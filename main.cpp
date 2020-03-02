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

struct qt_event : targeted_event
{
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

struct click_event : targeted_event
{
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
    handle_event<refresh_event>(ctx, [&](auto ctx, auto& e) {
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

template<class T>
struct cached_property
{
    // the value provided by the app
    keyed_data<T> value;
    // Does this need to be updated on the back end?
    bool dirty;
};

template<class T, class Signal>
void
refresh_property(cached_property<T>& cache, Signal property)
{
    refresh_keyed_data(cache.value, property.value_id());
    // TODO: Decide how to handle cases where the property has changed but isn't
    // immediately available (specifically w.r.t. the dirty flag).
    if (!is_valid(cache.value) && signal_has_value(property))
    {
        set(cache.value, read_signal(property));
        cache.dirty = true;
    }
}

struct qt_label : qt_layout_node
{
    std::shared_ptr<QLabel> object;
    cached_property<string> text;

    void
    update(alia::system* system, QWidget* parent, QLayout* layout)
    {
        if (!object)
        {
            // TODO: Allow changes in parent.
            object.reset(new QLabel(parent));
            if (parent->isVisible())
                object->show();
        }
        layout->addWidget(object.get());
        if (text.dirty)
        {
            object->setText(get(text.value).c_str());
            text.dirty = false;
        }
    }
};

static void
do_label(qt_context ctx, readable<string> text)
{
    qt_label* label;
    get_cached_data(ctx, &label);
    handle_event<refresh_event>(ctx, [&](auto ctx, auto& e) {
        refresh_property(label->text, text);
        add_layout_node(ctx, label);
    });
}

struct qt_button : qt_layout_node, node_identity
{
    std::unique_ptr<QPushButton> object;
    captured_id text_id;
    routing_region_ptr route;

    void
    update(alia::system* system, QWidget* parent, QLayout* layout)
    {
        layout->addWidget(object.get());
    }
};

template<class Signal, class OnNewValue, class OnLostValue>
void
refresh_signal_shadow(
    captured_id& id,
    Signal signal,
    OnNewValue&& on_new_value,
    OnLostValue&& on_lost_value)
{
    if (signal.has_value())
    {
        if (!id.matches(signal.value_id()))
        {
            on_new_value(signal.read());
            id.capture(signal.value_id());
        }
    }
    else
    {
        if (!id.matches(null_id))
        {
            on_lost_value();
            id.capture(null_id);
        }
    }
}

static void
do_button(qt_context ctx, readable<string> text, action<> on_click)
{
    auto& button = get_cached_data<qt_button>(ctx);

    handle_event<refresh_event>(ctx, [&](auto ctx, auto& e) {
        auto& system = get_component<system_tag>(ctx);

        auto& traversal = get_component<qt_traversal_tag>(ctx);
        auto* parent = traversal.active_parent;

        button.route = get_active_routing_region(ctx);

        if (!button.object)
        {
            button.object.reset(new QPushButton(parent));
            if (parent->isVisible())
                button.object->show();
            QObject::connect(
                button.object.get(),
                &QPushButton::clicked,
                // The Qt object is technically owned within both of these, so
                // I'm pretty sure it's safe to reference both.
                [&system, &button]() {
                    qt_event event;
                    dispatch_targeted_event(
                        system, event, routable_node_id{&button, button.route});
                    refresh_system(system);
                });
        }

        // Theoretically, this could change.
        if (button.object->parent() != parent)
            button.object->setParent(parent);

        add_layout_node(ctx, &button);

        refresh_signal_shadow(
            button.text_id,
            text,
            [&](auto text) { button.object->setText(text.c_str()); },
            [&]() { button.object->setText(""); });
    });

    handle_targeted_event<qt_event>(ctx, &button, [&](auto ctx, auto& e) {
        if (action_is_ready(on_click))
        {
            perform_action(on_click);
        }
    });
}

struct qt_text_control : qt_layout_node, node_identity
{
    std::shared_ptr<QTextEdit> object;
    routable_node_id node_id;
    cached_property<string> text;

    void
    update(alia::system* system, QWidget* parent, QLayout* layout)
    {
        if (!object)
        {
            // TODO: Allow changes in parent.
            object.reset(new QTextEdit(parent));
            if (parent->isVisible())
                object->show();
            auto node_id = this->node_id;
            QObject::connect(
                object.get(), &QTextEdit::textChanged, [system, node_id]() {
                    qt_event event;
                    dispatch_targeted_event(*system, event, node_id);
                    refresh_system(*system);
                });
        }
        layout->addWidget(object.get());
        if (text.dirty)
        {
            if (object->toPlainText().toUtf8().constData() != get(text.value))
                object->setText(get(text.value).c_str());
            text.dirty = false;
        }
    }
};

static void
do_text_control(qt_context ctx, bidirectional<string> text)
{
    qt_text_control* widget;
    get_cached_data(ctx, &widget);
    handle_event<refresh_event>(ctx, [&](auto ctx, auto& e) {
        widget->node_id = make_routable_node_id(ctx, widget);
        refresh_property(widget->text, text);
        add_layout_node(ctx, widget);
    });
    handle_targeted_event<qt_event>(ctx, widget, [&](auto ctx, auto& e) {
        write_signal(text, widget->object->toPlainText().toUtf8().constData());
    });
}

void
qt_system::operator()(alia::context vanilla_ctx)
{
    qt_traversal traversal;
    qt_context ctx = extend_context<qt_traversal_tag>(vanilla_ctx, traversal);

    if (is_refresh_event(ctx))
    {
        traversal.next_ptr = &this->root;
        traversal.active_parent = this->window;
    }

    this->controller(ctx);

    if (is_refresh_event(ctx))
    {
        while (this->layout->takeAt(0))
            ;
        this->root->update(this->system, this->window, this->layout);
    }
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
    // alia_system.external = &dom_system.external;
    alia_system.controller = std::ref(qt_system);
    qt_system.controller = std::move(controller);

    // Do the initial refresh.
    refresh_system(alia_system);
}

void
do_app_ui(qt_context ctx);

struct qt_column : qt_layout_container
{
    std::shared_ptr<QVBoxLayout> object;

    void
    update(alia::system* system, QWidget* parent, QLayout* layout)
    {
        if (!object)
        {
            // TODO: Allow changes in parent.
            object.reset(new QVBoxLayout(parent));
        }
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

    do_label(ctx, value("Hello, world!"));

    auto x = get_state(ctx, string());
    do_text_control(ctx, x);
    do_text_control(ctx, x);

    do_label(ctx, x);

    auto state = get_state(ctx, true);
    alia_if(state)
    {
        do_label(ctx, value("Secret message!"));
    }
    alia_end

        do_button(ctx, x, toggle(state));
    do_button(ctx, value("Toggle!"), toggle(state));
}
