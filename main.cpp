#include <QApplication>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QWidget>

#define ALIA_IMPLEMENTATION
#include "alia.hpp"

using namespace alia;

using std::string;

struct qt_layout_container;

struct qt_layout_node
{
    virtual void
    update(alia::system* system, QWidget* parent, QLayout* layout)
        = 0;

    qt_layout_node* next;
    qt_layout_container* parent;
};

struct qt_layout_container : qt_layout_node
{
    qt_layout_node* children;

    virtual void
    record_change();

    qt_layout_container* parent;

    bool dirty;
};

struct qt_traversal
{
    QWidget* active_parent = nullptr;
    qt_layout_container* active_container = nullptr;
    // a pointer to the pointer that should store the next item that's added
    qt_layout_node** next_ptr = nullptr;
};
ALIA_DEFINE_COMPONENT_TYPE(qt_traversal_tag, qt_traversal&)

typedef alia::add_component_type_t<alia::context, qt_traversal_tag> qt_context;

typedef alia::remove_component_type_t<qt_context, data_traversal_tag>
    dataless_qt_context;

struct qt_system
{
    alia::system* system;

    std::function<void(qt_context)> controller;

    // the root of the application's UI tree
    qt_layout_node* root;

    // the top-level window and layout for the UI - The entire application's UI
    // tree lives inside this.
    QWidget* window;
    QVBoxLayout* layout;

    void
    operator()(alia::context ctx);
};

void
record_layout_change(qt_traversal& traversal)
{
    if (traversal.active_container)
        traversal.active_container->record_change();
}

void
set_next_node(qt_traversal& traversal, qt_layout_node* node)
{
    if (*traversal.next_ptr != node)
    {
        record_layout_change(traversal);
        *traversal.next_ptr = node;
    }
}

void
add_layout_node(dataless_qt_context ctx, qt_layout_node* node)
{
    qt_traversal& traversal = get_component<qt_traversal_tag>(ctx);
    set_next_node(traversal, node);
    traversal.next_ptr = &node->next;
}

void
record_container_change(qt_layout_container* container)
{
    while (container && !container->dirty)
    {
        container->dirty = true;
        container = container->parent;
    }
}

void
qt_layout_container::record_change()
{
    record_container_change(this);
}

struct scoped_layout_container : noncopyable
{
    scoped_layout_container()
    {
    }
    scoped_layout_container(qt_context ctx, qt_layout_container* container)
    {
        begin(ctx, container);
    }
    ~scoped_layout_container()
    {
        end();
    }
    void
    begin(qt_context ctx, qt_layout_container* container);
    void
    end();

 private:
    qt_traversal* traversal_ = 0;
};

void
scoped_layout_container::begin(qt_context ctx, qt_layout_container* container)
{
    on_refresh(ctx, [&](auto ctx) {
        traversal_ = &get_component<qt_traversal_tag>(ctx);
        qt_traversal& traversal = *traversal_;

        set_next_node(traversal, container);
        container->parent = traversal.active_container;

        traversal.next_ptr = &container->children;
        traversal.active_container = container;
    });
}
void
scoped_layout_container::end()
{
    if (traversal_)
    {
        set_next_node(*traversal_, 0);

        qt_layout_container* container = traversal_->active_container;
        traversal_->next_ptr = &container->next;
        traversal_->active_container = container->parent;

        traversal_ = 0;
    }
}

struct qt_label : qt_layout_node
{
    std::shared_ptr<QLabel> object;
    captured_id text_id;

    void
    update(alia::system* system, QWidget* parent, QLayout* layout)
    {
        if (object->parent() != parent)
            object->setParent(parent);
        layout->addWidget(object.get());
    }
};

static void
do_label(qt_context ctx, readable<string> text)
{
    auto& label = get_cached_data<qt_label>(ctx);

    on_refresh(ctx, [&](auto ctx) {
        auto& system = get_component<system_tag>(ctx);

        if (!label.object)
        {
            auto& traversal = get_component<qt_traversal_tag>(ctx);
            auto* parent = traversal.active_parent;
            label.object.reset(new QLabel(parent));
            if (parent->isVisible())
                label.object->show();
        }

        add_layout_node(ctx, &label);

        refresh_signal_shadow(
            label.text_id,
            text,
            [&](auto text) { label.object->setText(text.c_str()); },
            [&]() { label.object->setText(""); });
    });
}

struct click_event : targeted_event
{
};

struct qt_button : qt_layout_node, node_identity
{
    std::unique_ptr<QPushButton> object;
    captured_id text_id;
    routing_region_ptr route;

    void
    update(alia::system* system, QWidget* parent, QLayout* layout)
    {
        if (object->parent() != parent)
            object->setParent(parent);
        layout->addWidget(object.get());
    }
};

static void
do_button(qt_context ctx, readable<string> text, action<> on_click)
{
    auto& button = get_cached_data<qt_button>(ctx);

    on_refresh(ctx, [&](auto ctx) {
        auto& system = get_component<system_tag>(ctx);

        button.route = get_active_routing_region(ctx);

        if (!button.object)
        {
            auto& traversal = get_component<qt_traversal_tag>(ctx);
            auto* parent = traversal.active_parent;
            button.object.reset(new QPushButton(parent));
            if (parent->isVisible())
                button.object->show();
            QObject::connect(
                button.object.get(),
                &QPushButton::clicked,
                // The Qt object is technically owned within both of these, so
                // I'm pretty sure it's safe to reference both.
                [&system, &button]() {
                    click_event event;
                    dispatch_targeted_event(
                        system, event, routable_node_id{&button, button.route});
                });
        }

        add_layout_node(ctx, &button);

        refresh_signal_shadow(
            button.text_id,
            text,
            [&](auto text) { button.object->setText(text.c_str()); },
            [&]() { button.object->setText(""); });
    });

    on_targeted_event<click_event>(ctx, &button, [&](auto ctx, auto& e) {
        if (action_is_ready(on_click))
        {
            perform_action(on_click);
        }
    });
}

struct value_update_event : targeted_event
{
    string value;
};

struct qt_text_control : qt_layout_node, node_identity
{
    std::shared_ptr<QTextEdit> object;
    captured_id text_id;
    routing_region_ptr route;

    void
    update(alia::system* system, QWidget* parent, QLayout* layout)
    {
        if (object->parent() != parent)
            object->setParent(parent);
        layout->addWidget(object.get());
    }
};

static void
do_text_control(qt_context ctx, bidirectional<string> text)
{
    auto& widget = get_cached_data<qt_text_control>(ctx);

    on_refresh(ctx, [&](auto ctx) {
        auto& system = get_component<system_tag>(ctx);

        widget.route = get_active_routing_region(ctx);

        if (!widget.object)
        {
            auto& traversal = get_component<qt_traversal_tag>(ctx);
            auto* parent = traversal.active_parent;
            widget.object.reset(new QTextEdit(parent));
            if (parent->isVisible())
                widget.object->show();
            QObject::connect(
                widget.object.get(),
                &QTextEdit::textChanged,
                // The Qt object is technically owned within both of these, so
                // I'm pretty sure it's safe to reference both.
                [&system, &widget]() {
                    value_update_event event;
                    event.value
                        = widget.object->toPlainText().toUtf8().constData();
                    dispatch_targeted_event(
                        system, event, routable_node_id{&widget, widget.route});
                });
        }

        add_layout_node(ctx, &widget);

        refresh_signal_shadow(
            widget.text_id,
            text,
            [&](auto text) {
                // Prevent update cycles.
                if (widget.object->toPlainText().toUtf8().constData() != text)
                    widget.object->setText(text.c_str());
            },
            [&]() {
                // Prevent update cycles.
                if (widget.object->toPlainText().toUtf8().constData() != "")
                    widget.object->setText("");
            });
    });

    on_targeted_event<value_update_event>(
        ctx, &widget, [&](auto ctx, auto& e) { write_signal(text, e.value); });
}

struct qt_column : qt_layout_container
{
    std::shared_ptr<QVBoxLayout> object;

    void
    update(alia::system* system, QWidget* parent, QLayout* layout)
    {
        if (!object)
            object.reset(new QVBoxLayout(parent));

        if (object->parent() != parent)
            object->setParent(parent);

        layout->addItem(object.get());

        if (this->dirty)
        {
            while (object->takeAt(0))
                ;
            for (auto* node = children; node; node = node->next)
            {
                node->update(system, parent, object.get());
            }
            this->dirty = false;
        }
    }
};

struct column_layout : noncopyable
{
    column_layout()
    {
    }
    column_layout(qt_context ctx)
    {
        begin(ctx);
    }
    ~column_layout()
    {
        end();
    }
    void
    begin(qt_context ctx)
    {
        qt_column* column;
        get_cached_data(ctx, &column);
        slc_.begin(ctx, column);
    }
    void
    end()
    {
        slc_.end();
    }

 private:
    scoped_layout_container slc_;
};

void
qt_system::operator()(alia::context vanilla_ctx)
{
    qt_traversal traversal;
    qt_context ctx = extend_context<qt_traversal_tag>(vanilla_ctx, traversal);

    on_refresh(ctx, [&](auto ctx) {
        traversal.next_ptr = &this->root;
        traversal.active_parent = this->window;
    });

    this->controller(ctx);

    on_refresh(ctx, [&](auto ctx) {
        while (this->layout->takeAt(0))
            ;
        this->root->update(this->system, this->window, this->layout);
    });
}

void
initialize(
    qt_system& qt_system,
    alia::system& alia_system,
    std::function<void(qt_context)> controller)
{
    // Initialize the Qt system.
    qt_system.system = &alia_system;
    qt_system.root = 0;
    qt_system.window = new QWidget;
    qt_system.layout = new QVBoxLayout(qt_system.window);
    qt_system.window->setLayout(qt_system.layout);

    // Hook up the Qt system to the alia system.
    alia_system.controller = std::ref(qt_system);
    qt_system.controller = std::move(controller);

    // Do the initial refresh.
    refresh_system(alia_system);
}

void
do_app_ui(qt_context ctx);

alia::system the_system;
qt_system the_qt;

int
main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    initialize(the_qt, the_system, do_app_ui);

    refresh_system(the_system);

    the_qt.window->setWindowTitle("alia Qt");
    the_qt.window->show();

    return app.exec();
}

void
do_app_ui(qt_context ctx)
{
    column_layout row(ctx);

    do_label(ctx, value("Hello, World!"));

    auto x = get_state(ctx, string());
    do_text_control(ctx, x);
    do_text_control(ctx, x);

    do_label(ctx, x);

    auto state = get_state(ctx, true);
    ALIA_IF(state)
    {
        do_label(ctx, value("Secret message!"));
    }
    ALIA_END

    do_button(ctx, x, toggle(state));
    do_button(ctx, value("Toggle!"), toggle(state));
}
