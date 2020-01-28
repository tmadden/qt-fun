#include <QApplication>
#include <QWidget>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QLayout>
#include <QTextEdit>

#include <alia/data_graph.hpp>
#include <alia/actions.hpp>

using namespace alia;

struct qt_layout_node;
struct qt_layout_container;
struct qt_context;

struct qt_system
{
    data_graph data;

    // the root of the application's UI tree
    qt_layout_node* root;

    // the top-level window and layout for the UI - The entire application's UI tree lives
    // inside this.
    QWidget* window;
    QVBoxLayout *layout;
};

struct qt_layout_node
{
    virtual void update(qt_system* system, QWidget* parent, QLayout* layout) = 0;

    qt_layout_node* next;
    qt_layout_container* parent;
};

struct qt_layout_container : qt_layout_node
{
    qt_layout_node* children;

    virtual void record_change();

    qt_layout_container* parent;

    bool dirty;
};

struct qt_event
{
    // This is just a hack for now. A null pointer here means that this is a refresh
    // event. A non-null pointer means that some event happened for that widget (and it
    // should be obvious what).
    QWidget* target;
};

struct qt_context
{
    alia::data_traversal* data;

    qt_system* system;

    qt_layout_container* active_container;
    // a pointer to the pointer that should store the next item that's added
    qt_layout_node** next_ptr;

    bool is_refresh_pass;

    qt_event* event;
};


void issue_ui_event(qt_system& system, QWidget* target);
void update_ui(qt_system& system);


void record_layout_change(qt_context& traversal)
{
    if (traversal.active_container)
        traversal.active_container->record_change();
}

void set_next_node(qt_context& traversal, qt_layout_node* node)
{
    if (*traversal.next_ptr != node)
    {
        record_layout_change(traversal);
        *traversal.next_ptr = node;
    }
}

void add_layout_node(qt_context& traversal, qt_layout_node* node)
{
    set_next_node(traversal, node);
    traversal.next_ptr = &node->next;
}

void record_container_change(qt_layout_container* container)
{
    while (container && !container->dirty)
    {
        container->dirty = true;
        container = container->parent;
    }
}

void qt_layout_container::record_change()
{
    record_container_change(this);
}

struct scoped_layout_container : noncopyable
{
    scoped_layout_container() : traversal_(0) {}
    scoped_layout_container(qt_context& traversal, qt_layout_container* container)
    { begin(traversal, container); }
    ~scoped_layout_container() { end(); }
    void begin(qt_context& traversal, qt_layout_container* container);
    void end();
 private:
    qt_context* traversal_;
};

void scoped_layout_container::begin(
    qt_context& traversal, qt_layout_container* container)
{
    if (traversal.is_refresh_pass)
    {
        traversal_ = &traversal;

        set_next_node(traversal, container);
        container->parent = traversal.active_container;

        traversal.next_ptr = &container->children;
        traversal.active_container = container;
    }
}
void scoped_layout_container::end()
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


alia::data_traversal& get_data_traversal(qt_context& ctx)
{ return *ctx.data; }

template<class T>
struct cached_property
{
    // the value provided by the app
    keyed_data<T> value;
    // Does this need to be updated on the back end?
    bool dirty;
};

template<class T>
void refresh_property(cached_property<T>& cache, accessor<T> const& property)
{
    refresh_keyed_data(cache.value, property.id());
    // TODO: Decide how to handle cases where the property has changed but isn't
    // immediately available (specifically w.r.t. the dirty flag).
    if (!is_valid(cache.value) && property.is_gettable())
    {
        set(cache.value, get(property));
        cache.dirty = true;
    }
}

struct qt_label : qt_layout_node
{
    std::shared_ptr<QLabel> object;
    cached_property<string> text;

    void update(qt_system* system, QWidget* parent, QLayout* layout)
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
do_label(qt_context& ctx, accessor<string> const& text)
{
    qt_label* label;
    get_cached_data(ctx, &label);
    if (ctx.is_refresh_pass)
    {
        refresh_property(label->text, text);
        add_layout_node(ctx, label);
    }
}

struct qt_button : qt_layout_node
{
    std::shared_ptr<QPushButton> object;
    cached_property<string> text;

    void update(qt_system* system, QWidget* parent, QLayout* layout)
    {
        if (!object)
        {
            // TODO: Allow changes in parent.
            object.reset(new QPushButton(parent));
            if (parent->isVisible())
                object->show();
            auto target = object.get();
            QObject::connect(object.get(), &QPushButton::clicked,
                [system,target]()
                {
                    issue_ui_event(*system, target);
                    update_ui(*system);
                });
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
do_button(
    qt_context& ctx,
    accessor<string> const& text,
    action const& on_click)
{
    qt_button* button;
    get_cached_data(ctx, &button);
    if (ctx.is_refresh_pass)
    {
        refresh_property(button->text, text);
        add_layout_node(ctx, button);
    }
    if (button->object && ctx.event->target == button->object.get() && on_click.is_ready())
    {
        on_click.perform();
        //end_pass(ctx);
    }
}

struct qt_text_control : qt_layout_node
{
    std::shared_ptr<QTextEdit> object;
    cached_property<string> text;

    void update(qt_system* system, QWidget* parent, QLayout* layout)
    {
        if (!object)
        {
            // TODO: Allow changes in parent.
            object.reset(new QTextEdit(parent));
            if (parent->isVisible())
                object->show();
            auto target = object.get();
            QObject::connect(object.get(), &QTextEdit::textChanged,
                [system,target]()
                {
                    issue_ui_event(*system, target);
                    update_ui(*system);
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
do_text_control(
    qt_context& ctx,
    accessor<string> const& text)
{
    qt_text_control* widget;
    get_cached_data(ctx, &widget);
    if (ctx.is_refresh_pass)
    {
        refresh_property(widget->text, text);
        add_layout_node(ctx, widget);
    }
    if (widget->object && ctx.event->target == widget->object.get())
    {
        set(text, widget->object->toPlainText().toUtf8().constData());
        //end_pass(ctx);
    }
}

qt_system the_system;

void initialize_ui(qt_system& system)
{
    system.root = 0;
    system.window = new QWidget;
    system.layout = new QVBoxLayout(system.window);
    system.window->setLayout(system.layout);
}

void do_app_ui(qt_context& ctx);

void issue_ui_event(qt_system& system, QWidget* target)
{
    qt_context ctx;

    bool is_refresh = target == 0;

    ctx.system = &system;
    ctx.active_container = 0;
    ctx.next_ptr = &system.root;
    ctx.is_refresh_pass = is_refresh;

    qt_event event;
    event.target = target;
    ctx.event = &event;

    data_traversal data;
    scoped_data_traversal sdt(system.data, data);
    ctx.data = &data;

    // Only use refresh events to decide when data is no longer needed.
    data.gc_enabled = data.cache_clearing_enabled = is_refresh;

    do_app_ui(ctx);
}

void refresh_ui(qt_system& system)
{
    issue_ui_event(system, 0);
}

struct qt_column : qt_layout_container
{
    std::shared_ptr<QVBoxLayout> object;

    void update(qt_system* system, QWidget* parent, QLayout* layout)
    {
        if (!object)
        {
            // TODO: Allow changes in parent.
            object.reset(new QVBoxLayout(parent));
        }
        layout->addItem(object.get());
        while (object->takeAt(0))
            ;
        for (auto* node = children; node; node = node->next)
        {
            node->update(system, parent, object.get());
        }
    }
};

struct column_layout : noncopyable
{
    column_layout() {}
    column_layout(qt_context& ctx){ begin(ctx); }
    ~column_layout() { end(); }
    void begin(qt_context& ctx)
    {
        qt_column* column;
        get_cached_data(ctx, &column);
        slc_.begin(ctx, column);
    }
    void end() { slc_.end(); }
 private:
    scoped_layout_container slc_;
};

void update_ui(qt_system& system)
{
    refresh_ui(system);
    while (system.layout->takeAt(0))
        ;
    system.root->update(&system, system.window, system.layout);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    initialize_ui(the_system);

    update_ui(the_system);

    the_system.window->setWindowTitle("alia Qt");
    the_system.window->show();

    return app.exec();
}

void do_app_ui(qt_context& ctx)
{
    column_layout row(ctx);

    do_label(ctx, text("Hello, world!"));

    auto x = get_state(ctx, string());
    do_text_control(ctx, x);
    do_text_control(ctx, x);

    do_label(ctx, x);

    auto state = get_state(ctx, true);
    alia_if (state)
    {
        do_label(ctx, text("Secret message!"));
    }
    alia_end

    do_button(ctx, text("Toggle!"), make_toggle_action(state));
}
