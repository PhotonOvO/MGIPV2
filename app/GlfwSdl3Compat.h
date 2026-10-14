#pragma once

// ---------------------------------------------------------------------------
// GLFW-shaped window/input layer implemented on SDL3.
//
// The scene editor's window and input code predates the SDL3 migration. Rather
// than rewrite 68 call sites and the callback model in one unverifiable step,
// this header re-expresses the small GLFW surface the app layer uses on top of
// SDL3. The GLFW dependency is gone from the build: every entry point below is
// SDL3. Native SDL3 call sites can replace these incrementally.
//
// Callback model: GLFW delivers input through per-window callbacks, SDL3
// delivers it through a single event queue. glfwPollEvents() drains the queue,
// forwards every event to the ImGui SDL3 backend, then dispatches to the
// callbacks registered here.
// ---------------------------------------------------------------------------

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace mgif_sdl
{
	// ---- constants (values the app layer references) ----------------------
	enum
	{
		GLFW_TRUE_ = 1,
		GLFW_FALSE_ = 0,
		GLFW_PRESS_ = 1,
		GLFW_RELEASE_ = 0,
		GLFW_REPEAT_ = 2,
	};

	// ---- window state ----------------------------------------------------
	struct WindowState
	{
		SDL_Window* window{nullptr};
		bool shouldClose{false};
		bool decorated{true};

		void (*keyCallback)(SDL_Window*, int, int, int, int){nullptr};
		void (*mouseButtonCallback)(SDL_Window*, int, int, int){nullptr};
		void (*focusCallback)(SDL_Window*, int){nullptr};
		void (*dropCallback)(SDL_Window*, int, const char**){nullptr};

		// Virtual cursor position. GLFW reports a monotonic position even while
		// the cursor is captured; SDL3 reports a delta. Accumulating the delta
		// keeps the app layer's "position minus previous position" math intact.
		double cursorX{0.0};
		double cursorY{0.0};
	};

	inline WindowState& state()
	{
		static WindowState s;
		return s;
	}

	// Set by the app once the ImGui SDL3 backend is initialized.
	inline void (*eventSink)(const SDL_Event*){nullptr};

	inline int toGlfwButton(Uint8 sdlButton)
	{
		switch (sdlButton)
		{
		case SDL_BUTTON_LEFT: return 0;
		case SDL_BUTTON_RIGHT: return 1;
		case SDL_BUTTON_MIDDLE: return 2;
		default: return -1;
		}
	}
} // namespace mgif_sdl

// ---- types ---------------------------------------------------------------
using GLFWwindow = SDL_Window;

struct GLFWvidmode
{
	int width{0};
	int height{0};
	int redBits{8};
	int greenBits{8};
	int blueBits{8};
	int refreshRate{0};
};

struct GLFWmonitor
{
	SDL_DisplayID id{0};
};

// ---- constants -----------------------------------------------------------
#define GLFW_TRUE mgif_sdl::GLFW_TRUE_
#define GLFW_FALSE mgif_sdl::GLFW_FALSE_
#define GLFW_PRESS mgif_sdl::GLFW_PRESS_
#define GLFW_RELEASE mgif_sdl::GLFW_RELEASE_
#define GLFW_REPEAT mgif_sdl::GLFW_REPEAT_

#define GLFW_CLIENT_API 0x00022001
#define GLFW_NO_API 0
#define GLFW_DECORATED 0x00020005
#define GLFW_FLOATING 0x00020007
#define GLFW_FOCUSED 0x00020001
#define GLFW_ICONIFIED 0x00020002

#define GLFW_CURSOR 0x00033001
#define GLFW_CURSOR_NORMAL 0x00034001
#define GLFW_CURSOR_DISABLED 0x00034003

#define GLFW_MOUSE_BUTTON_LEFT 0
#define GLFW_MOUSE_BUTTON_RIGHT 1
#define GLFW_MOUSE_BUTTON_MIDDLE 2

// GLFW key tokens are SDL scancodes: both name physical keys.
#define GLFW_KEY_ESCAPE SDL_SCANCODE_ESCAPE
#define GLFW_KEY_W SDL_SCANCODE_W
#define GLFW_KEY_A SDL_SCANCODE_A
#define GLFW_KEY_S SDL_SCANCODE_S
#define GLFW_KEY_D SDL_SCANCODE_D
#define GLFW_KEY_Q SDL_SCANCODE_Q
#define GLFW_KEY_E SDL_SCANCODE_E
#define GLFW_KEY_F1 SDL_SCANCODE_F1
#define GLFW_KEY_LEFT_SHIFT SDL_SCANCODE_LSHIFT
#define GLFW_KEY_RIGHT_SHIFT SDL_SCANCODE_RSHIFT

// ---- window creation / lifetime -----------------------------------------
namespace mgif_sdl
{
	inline std::unordered_map<int, int>& hints()
	{
		static std::unordered_map<int, int> h;
		return h;
	}
} // namespace mgif_sdl

inline void glfwWindowHint(int hint, int value)
{
	mgif_sdl::hints()[hint] = value;
}

inline GLFWwindow* glfwCreateWindow(int width, int height, const char* title, GLFWmonitor* /*monitor*/,
                                    GLFWwindow* /*share*/)
{
	SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (auto it = mgif_sdl::hints().find(GLFW_DECORATED);
		it != mgif_sdl::hints().end() && it->second == GLFW_FALSE)
	{
		flags |= SDL_WINDOW_BORDERLESS;
	}
	SDL_Window* window = SDL_CreateWindow(title, width, height, flags);
	mgif_sdl::state().window = window;
	mgif_sdl::state().decorated = (flags & SDL_WINDOW_BORDERLESS) == 0;
	return window;
}

inline void glfwDestroyWindow(GLFWwindow* window)
{
	if (window != nullptr)
	{
		SDL_DestroyWindow(window);
	}
	if (mgif_sdl::state().window == window)
	{
		mgif_sdl::state().window = nullptr;
	}
}

inline int glfwWindowShouldClose(GLFWwindow* /*window*/)
{
	return mgif_sdl::state().shouldClose ? GLFW_TRUE : GLFW_FALSE;
}

inline void glfwSetWindowShouldClose(GLFWwindow* /*window*/, int value)
{
	mgif_sdl::state().shouldClose = value != GLFW_FALSE;
}

// ---- user pointer --------------------------------------------------------
inline void glfwSetWindowUserPointer(GLFWwindow* window, void* pointer)
{
	SDL_SetWindowData(window, "mgif_user_pointer", pointer);
}

inline void* glfwGetWindowUserPointer(GLFWwindow* window)
{
	return SDL_GetWindowData(window, "mgif_user_pointer");
}

// ---- callbacks -----------------------------------------------------------
inline void glfwSetKeyCallback(GLFWwindow* /*window*/, void (*callback)(GLFWwindow*, int, int, int, int))
{
	mgif_sdl::state().keyCallback = callback;
}

inline void glfwSetMouseButtonCallback(GLFWwindow* /*window*/, void (*callback)(GLFWwindow*, int, int, int))
{
	mgif_sdl::state().mouseButtonCallback = callback;
}

inline void glfwSetWindowFocusCallback(GLFWwindow* /*window*/, void (*callback)(GLFWwindow*, int))
{
	mgif_sdl::state().focusCallback = callback;
}

inline void glfwSetDropCallback(GLFWwindow* /*window*/, void (*callback)(GLFWwindow*, int, const char**))
{
	mgif_sdl::state().dropCallback = callback;
}

// ---- event pump ----------------------------------------------------------
inline void glfwPollEvents()
{
	mgif_sdl::WindowState& s = mgif_sdl::state();
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		if (mgif_sdl::eventSink != nullptr)
		{
			mgif_sdl::eventSink(&event);
		}

		const bool fromMainWindow =
			event.type == SDL_EVENT_QUIT || event.window.windowID == SDL_GetWindowID(s.window);

		switch (event.type)
		{
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			if (fromMainWindow)
			{
				s.shouldClose = true;
			}
			break;

		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			if (s.keyCallback != nullptr)
			{
				s.keyCallback(s.window, static_cast<int>(event.key.scancode), 0,
				              event.key.down ? GLFW_PRESS : GLFW_RELEASE, event.key.repeat ? GLFW_REPEAT : 0);
			}
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			if (s.mouseButtonCallback != nullptr)
			{
				const int button = mgif_sdl::toGlfwButton(event.button.button);
				if (button >= 0)
				{
					s.mouseButtonCallback(s.window, button,
					                      event.button.down ? GLFW_PRESS : GLFW_RELEASE, 0);
				}
			}
			break;

		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			if (s.focusCallback != nullptr)
			{
				s.focusCallback(s.window, GLFW_TRUE);
			}
			break;

		case SDL_EVENT_WINDOW_FOCUS_LOST:
			if (s.focusCallback != nullptr)
			{
				s.focusCallback(s.window, GLFW_FALSE);
			}
			break;

		case SDL_EVENT_DROP_FILE:
		{
			if (s.dropCallback != nullptr && event.drop.data != nullptr)
			{
				const char* paths[1] = {event.drop.data};
				s.dropCallback(s.window, 1, paths);
			}
			break;
		}

		default:
			break;
		}
	}
}

// ---- queries -------------------------------------------------------------
inline int glfwGetKey(GLFWwindow* /*window*/, int key)
{
	if (key < 0 || key >= SDL_SCANCODE_COUNT)
	{
		return GLFW_RELEASE;
	}
	const bool* keys = SDL_GetKeyboardState(nullptr);
	return (keys != nullptr && keys[key]) ? GLFW_PRESS : GLFW_RELEASE;
}

inline int glfwGetMouseButton(GLFWwindow* /*window*/, int button)
{
	const SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
	Uint32 mask = 0;
	switch (button)
	{
	case GLFW_MOUSE_BUTTON_LEFT: mask = SDL_BUTTON_LMASK; break;
	case GLFW_MOUSE_BUTTON_RIGHT: mask = SDL_BUTTON_RMASK; break;
	case GLFW_MOUSE_BUTTON_MIDDLE: mask = SDL_BUTTON_MMASK; break;
	default: return GLFW_RELEASE;
	}
	return (buttons & mask) != 0 ? GLFW_PRESS : GLFW_RELEASE;
}

inline void glfwGetCursorPos(GLFWwindow* window, double* x, double* y)
{
	mgif_sdl::WindowState& s = mgif_sdl::state();
	if (SDL_GetWindowRelativeMouseMode(window))
	{
		float dx = 0.0f;
		float dy = 0.0f;
		SDL_GetRelativeMouseState(&dx, &dy);
		s.cursorX += dx;
		s.cursorY += dy;
	}
	else
	{
		float mx = 0.0f;
		float my = 0.0f;
		SDL_GetMouseState(&mx, &my);
		s.cursorX = mx;
		s.cursorY = my;
	}
	if (x != nullptr)
	{
		*x = s.cursorX;
	}
	if (y != nullptr)
	{
		*y = s.cursorY;
	}
}

inline void glfwSetInputMode(GLFWwindow* window, int mode, int value)
{
	if (mode == GLFW_CURSOR)
	{
		SDL_SetWindowRelativeMouseMode(window, value == GLFW_CURSOR_DISABLED);
	}
}

inline int glfwGetWindowAttrib(GLFWwindow* window, int attrib)
{
	const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
	switch (attrib)
	{
	case GLFW_FOCUSED: return (flags & SDL_WINDOW_INPUT_FOCUS) != 0 ? GLFW_TRUE : GLFW_FALSE;
	case GLFW_ICONIFIED: return (flags & SDL_WINDOW_MINIMIZED) != 0 ? GLFW_TRUE : GLFW_FALSE;
	case GLFW_DECORATED: return (flags & SDL_WINDOW_BORDERLESS) != 0 ? GLFW_FALSE : GLFW_TRUE;
	case GLFW_FLOATING: return (flags & SDL_WINDOW_ALWAYS_ON_TOP) != 0 ? GLFW_TRUE : GLFW_FALSE;
	default: return GLFW_FALSE;
	}
}

inline void glfwSetWindowAttrib(GLFWwindow* window, int attrib, int value)
{
	switch (attrib)
	{
	case GLFW_FLOATING: SDL_SetWindowAlwaysOnTop(window, value == GLFW_TRUE); break;
	case GLFW_DECORATED: SDL_SetWindowBordered(window, value == GLFW_TRUE); break;
	default: break;
	}
}

inline void glfwGetFramebufferSize(GLFWwindow* window, int* width, int* height)
{
	SDL_GetWindowSizeInPixels(window, width, height);
}

inline void glfwGetWindowSize(GLFWwindow* window, int* width, int* height)
{
	SDL_GetWindowSize(window, width, height);
}

inline void glfwGetWindowPos(GLFWwindow* window, int* x, int* y)
{
	SDL_GetWindowPosition(window, x, y);
}

inline void glfwGetWindowContentScale(GLFWwindow* window, float* xScale, float* yScale)
{
	const float scale = SDL_GetWindowDisplayScale(window);
	if (xScale != nullptr)
	{
		*xScale = scale > 0.0f ? scale : 1.0f;
	}
	if (yScale != nullptr)
	{
		*yScale = scale > 0.0f ? scale : 1.0f;
	}
}

inline void glfwFocusWindow(GLFWwindow* window)
{
	SDL_RaiseWindow(window);
}

inline GLFWmonitor* glfwGetPrimaryMonitor()
{
	static GLFWmonitor monitor{SDL_GetPrimaryDisplay()};
	return &monitor;
}

inline const GLFWvidmode* glfwGetVideoMode(GLFWmonitor* monitor)
{
	static GLFWvidmode mode{};
	const SDL_DisplayID display = monitor != nullptr ? monitor->id : SDL_GetPrimaryDisplay();
	const SDL_DisplayMode* current = SDL_GetDesktopDisplayMode(display);
	if (current == nullptr)
	{
		return nullptr;
	}
	mode.width = static_cast<int>(current->w);
	mode.height = static_cast<int>(current->h);
	mode.refreshRate = static_cast<int>(current->refresh_rate);
	return &mode;
}

inline void glfwSetWindowMonitor(GLFWwindow* window, GLFWmonitor* monitor, int x, int y, int width, int height,
                                 int /*refreshRate*/)
{
	if (monitor == nullptr)
	{
		SDL_SetWindowFullscreen(window, false);
		SDL_SetWindowPosition(window, x, y);
		SDL_SetWindowSize(window, width, height);
	}
	else
	{
		SDL_SetWindowFullscreen(window, true);
	}
}

inline int glfwVulkanSupported()
{
	return GLFW_TRUE;
}

#if defined(_WIN32)
inline HWND glfwGetWin32Window(GLFWwindow* window)
{
	return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
	                                                SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}
#endif

#if defined(__APPLE__)
inline void* glfwGetCocoaWindow(GLFWwindow* window)
{
	return SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
	                              nullptr);
}
#endif
