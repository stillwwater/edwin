Edwin
=====

A small graphical user interface library for creating debug tools for Windows applications.

![demo screenshot](res/screen.png)

Usage
-----

![counter](res/counter.png)

~~~cpp
#include "edwin.h"

static ed_node *counter;
static int count;

void
count_onclick(ed_node *button)
{
    ++count;

    // Update counter input (redraw).
    ed_data(counter, &count);
}

void
counter_example()
{
    // Create a centered window.
    ed_begin_window("Example", {0.5f, 0.5f, 200, 100});
    {
        // Create a labeled input field.
        counter = ed_int("Counter");

        // Create a button taking up 100% of the parent width.
        ed_button("Count", count_onclick, {0, 0, 1, 0});
    }
    ed_end();
}
~~~

Installation
------------

Build `edwin.cpp` and link it with your executable. Requires a C++ 17 compiler or newer.

~~~
cl /W4 /std:c++20 /c edwin.cpp
link /subsystem:windows user32.lib gdi32.lib comctl32.lib msimg32.lib edwin.obj your_app.obj
~~~
