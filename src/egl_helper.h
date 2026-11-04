#ifndef EGL_HELPER_H
#define EGL_HELPER_H

/* The engine never touches EGL: on Android the context belongs to the Java
 * GLSurfaceView. We create it here instead, on the main thread, and run the
 * whole game loop from that same thread so GL calls never cross threads. */

void egl_init(int w, int h);
void egl_swap(void);
void egl_deinit(void);

/* a context can only be current on one thread: release it before handing it
 * to another, then bind it there */
void egl_release_thread(void);

/* make the existing context current on the calling thread */
int egl_bind_thread(void);
/* dock/undock: recreate the window surface at the new size without tearing
 * down the GL context (the engine keeps its textures) */
void egl_resize(int w, int h);

#endif
