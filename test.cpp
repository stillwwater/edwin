#include <windows.h>

#include "edwin.h"

const char *options[] = {"A", "B", "C", "D"};
char scratch[16][16];
int si = 0;

static LRESULT __stdcall
WindowProc(HWND hwnd, UINT u_msg, WPARAM w_param, LPARAM l_param)
{
    switch (u_msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        FillRect(hdc, &ps.rcPaint, GetSysColorBrush(COLOR_WINDOW));
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_SIZE:
        // Resize UI tree
        ed_resize(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, u_msg, w_param, l_param);
}

void
test_rect(ed_rect rect)
{
    ed_begin_border(rect);
    ed_end();
}

int __stdcall
WinMain(HINSTANCE hinstance, HINSTANCE, LPSTR, int cmdshow)
{
    constexpr int width = 800;
    constexpr int height = 600;

    // Main window of your existing windows application
    WNDCLASS wndclass = {};
    wndclass.lpfnWndProc   = WindowProc;
    wndclass.hInstance     = hinstance;
    wndclass.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wndclass.lpszClassName = "ED_TEST";
    RegisterClass(&wndclass);

    HWND hwnd = CreateWindow("ED_TEST", "Edwin Test", WS_OVERLAPPEDWINDOW,
            (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
            (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
            width, height, NULL, NULL, hinstance, NULL);

    ShowWindow(hwnd, cmdshow);

    ed_init(hwnd);
    ed_begin_window("Everything", {0, 0, 0.4f, 1});
    {
        ed_begin_border({0, 0, 1, 0});
        {
            ed_label("Below are all edwin controls");
        }
        ed_end();

        ed_begin_group("All Controls");
        {
            ed_button("Button", NULL);
            ed_space(20);
            ed_string("String");
            ed_int("Int");
            ed_data(ed_int("Hex Int", 0, 0, "0x%X"), scratch[si++]);
            ed_float("Float");
            ed_float("Float Slider", 0, 1);
            ed_int64("Int64");
            ed_float64("Float64");
            ed_data(ed_enum("Enum", options, 4), scratch[si++]);
            ed_data(ed_flags("Flags", options, 4), scratch[si++]);
            ed_data(ed_bool("Bool"), scratch[si++]);
            ed_text("Text");
            ed_vector("Vector", ED_FLOAT, 3);
            ed_matrix("Matrix", ED_FLOAT, 3, 3);
            ed_color("Color");
            ed_image((unsigned char *)scratch[si++], 1, 1, ED_RGB, {1, 0, 128, 128});
        }
        ed_end();

        ed_begin_group("Group");
        {
            ed_begin_border({0, 0, 1, 0});
            {
                ed_label("That's all");
            }
            ed_end();
        }
        ed_end();
    }
    ed_end();

    ed_begin_window("Layout Test", {1, 0, 0.6f, 1});
    {
        ed_begin_border({0, 0, 1.0f, 0});
        {
            test_rect({0, 0, 1.0, 30});
            test_rect({0, 0, 1.0, 20});
            ed_begin_border({0, 0, 1.0f, 0}, ED_HORZ);
            {
                test_rect({0, 0, 0.333f, 20});
                test_rect({0, 0, 0.333f, 20});
                test_rect({0, 0, 0.333f, 20});
            }
            ed_end();
            ed_begin_border({0, 0, 1.0f, 0}, ED_HORZ);
            {
                test_rect({0, 0, 1, 20});
                test_rect({0, 0, 50, 20});
                test_rect({0, 0, 50, 20});
            }
            ed_end();
        }
        ed_end();
        ed_begin_border({0, 0, 1.0, 1.0});
        {
            ed_begin_border({0, 0, 1.0, 0}, ED_HORZ);
            {
                test_rect({0, 0, 20, 20});
                test_rect({0, 0, 1.0, 20});
                test_rect({0, 0, 20, 20});
            }
            ed_end();
            ed_begin_border({0, 0, 1, 1}, ED_ABS);
            {
                test_rect({0, 0, 40, 40});
                test_rect({1, 0, 40, 40});
                test_rect({0, 1, 40, 40});
                test_rect({1, 1, 40, 40});
                test_rect({0.5, 1, 40, 40});
                test_rect({1, 0.5, 40, 40});
                test_rect({0.5, 0, 40, 40});
                test_rect({0, 0.5, 40, 40});
                test_rect({0.5, 0.5, 40, 40});
            }
            ed_end();
            ed_begin_border({0, 0, 1.0, 0}, ED_HORZ);
            {
                test_rect({0, 0, 20, 20});
                test_rect({0, 0, 1.0, 20});
                test_rect({0, 0, 20, 20});
            }
            ed_end();
        }
        ed_end();
    }
    ed_end();

    MSG msg = {};
    for (;;) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                break;
            }

            continue;
        }

        // Update/render your game
        Sleep(16);
    }

    ed_deinit();
    return 0;
}
