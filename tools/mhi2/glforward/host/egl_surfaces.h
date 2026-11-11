/*
 * Opt-in EGL lifecycle protocol: eight independent contexts/surfaces per PID.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GLHOST_EGL_SURFACES_H
#define GLHOST_EGL_SURFACES_H

static struct ClientSurface *client_surface(struct Client *c, uint32_t id)
{
    for (unsigned i = 0; i < 8; i++) {
        if (id && c->targets[i].id == id) {
            return &c->targets[i];
        }
    }
    return NULL;
}

/* 139: {action, id, a, b, thread, reserved}; reply is an EGL error code. */
static uint32_t client_egl_control(uint32_t pid, const uint8_t *p)
{
    struct Client *c = get_client(pid);
    struct Client *context, *previous;
    uint32_t action = u32(p, 0), id = u32(p, 4);
    uint32_t a = u32(p, 8), b = u32(p, 12), thread = u32(p, 16);
    struct ClientSurface *target, *draw, *read;

    if (!c) {
        return EGL_BAD_ALLOC;
    }
    if (u32(p, 20) || (action != 5 && thread)) {
        return EGL_BAD_PARAMETER;
    }
    if (action == 0) {
        if (id || a || b) {
            return EGL_BAD_PARAMETER;
        }
        c->managed = true;
        return EGL_SUCCESS;
    }
    if (!c->managed) {
        return EGL_NOT_INITIALIZED;
    }
    switch (action) {
    case 1:
        if (!id || (a != 2 && a != 3) || b) {
            return EGL_BAD_MATCH;
        }
        if (client_context(c, id)) return EGL_BAD_ALLOC;
        context = NULL;
        for (unsigned i = 0; i < 8; i++) {
            struct Client *candidate = client_context_at(c, i);
            if (candidate && !candidate->context_id) {
                context = candidate;
                break;
            }
            if (!candidate) {
                candidate = calloc(1, sizeof(*candidate));
                if (!candidate) return EGL_BAD_ALLOC;
                if (!client_storage(candidate)) {
                    free(candidate);
                    return EGL_BAD_ALLOC;
                }
                candidate->pid = pid;
                candidate->managed = true;
                c->contexts[i - 1] = candidate;
                context = candidate;
                break;
            }
        }
        if (!context) return EGL_BAD_ALLOC;
        {
            const EGLint attributes[] = {
                EGL_CONTEXT_CLIENT_VERSION, a, EGL_NONE,
            };
            if (context->context && !eglDestroyContext(display, context->context)) {
                return eglGetError();
            }
            context->context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                         attributes);
            if (!context->context) {
                return eglGetError();
            }
        }
        context->context_id = id;
        return EGL_SUCCESS;
    case 2:
        context = client_context(c, id);
        if (a || b || !context) {
            return EGL_BAD_CONTEXT;
        }
        if (context->owner || context->images) {
            return EGL_BAD_ACCESS;
        }
        if (!eglDestroyContext(display, context->context)) {
            return eglGetError();
        }
        context->context = EGL_NO_CONTEXT;
        context->context_id = 0;
        context->saved_errors = 0;
        for (unsigned i = 0; i < 16; i++) {
            free(context->blend->client_arrays[i]);
        }
        free(context->blend->menu);
        memset(context->blend, 0, sizeof(*context->blend));
        memset(context->objects, 0, MAXOBJ * sizeof(*context->objects));
        current_pid = 0;
        current_client = NULL;
        return EGL_SUCCESS;
    case 3:
        if (!id || client_surface(c, id)) {
            return EGL_BAD_SURFACE;
        }
        if (!a || a > 2048 || !b || b > 2048) {
            return EGL_BAD_PARAMETER;
        }
        target = NULL;
        for (unsigned i = 0; i < 8; i++) {
            if (!c->targets[i].id) {
                target = &c->targets[i];
                break;
            }
        }
        if (!target) {
            return EGL_BAD_ALLOC;
        }
        {
            const EGLint attributes[] = {
                EGL_WIDTH, a, EGL_HEIGHT, b, EGL_NONE,
            };
            EGLSurface created = eglCreatePbufferSurface(display, config,
                                                         attributes);
            if (!created) {
                return eglGetError();
            }
            *target = (struct ClientSurface) {
                .id = id, .surface = created, .width = a, .height = b,
            };
        }
        return EGL_SUCCESS;
    case 4:
        target = client_surface(c, id);
        if (!target || a || b) {
            return EGL_BAD_SURFACE;
        }
        for (unsigned i = 0; i < 8; i++) {
            context = client_context_at(c, i);
            if (context && (target == context->draw || target == context->read)) {
                return EGL_BAD_ACCESS;
            }
        }
        if (!eglDestroySurface(display, target->surface)) {
            return eglGetError();
        }
        memset(target, 0, sizeof(*target));
        return EGL_SUCCESS;
    case 5:
        if (!thread) return EGL_BAD_ACCESS;
        context = client_context(c, id);
        previous = bound_client(pid, thread);
        if (id && !context) {
            return EGL_BAD_CONTEXT;
        }
        if (context && context->owner && context->owner != thread)
            return EGL_BAD_ACCESS;
        draw = client_surface(c, a);
        read = client_surface(c, b);
        if ((a && !draw) || (b && !read)) {
            return EGL_BAD_SURFACE;
        }
        if ((!id && (a || b)) || (!a != !b)) {
            return EGL_BAD_MATCH;
        }
        for (unsigned i = 0; i < 8; i++) {
            struct Client *other = client_context_at(c, i);
            if (other && other->owner && other->owner != thread &&
                ((draw && (other->draw == draw || other->read == draw)) ||
                 (read && (other->draw == read || other->read == read))))
                return EGL_BAD_ACCESS;
        }
        if (!eglMakeCurrent(display, draw ? draw->surface : EGL_NO_SURFACE,
                            read ? read->surface : EGL_NO_SURFACE,
                            context ? context->context : EGL_NO_CONTEXT)) {
            return eglGetError();
        }
        if (previous) {
            previous->owner = 0;
            previous->draw = previous->read = NULL;
        }
        if (context) {
            context->owner = thread;
            context->draw = draw;
            context->read = read;
        }
        current_pid = 0;
        current_client = NULL;
        surface = EGL_NO_SURFACE;
        return EGL_SUCCESS;
    default:
        return EGL_BAD_PARAMETER;
    }
}

#endif
