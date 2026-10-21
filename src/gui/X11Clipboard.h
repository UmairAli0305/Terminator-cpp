#ifndef TERMINATOR_X11_CLIPBOARD_H
#define TERMINATOR_X11_CLIPBOARD_H
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <string>

// CLIPBOARD selection, handled in the X11 event loop (no subprocesses).
class X11Clipboard {
    Display* display;
    Window window;
    Atom clipboard, utf8, targets, property;
    std::string owned;
public:
    X11Clipboard(Display* d, Window w) : display(d), window(w) {
        clipboard = XInternAtom(d, "CLIPBOARD", False);
        utf8 = XInternAtom(d, "UTF8_STRING", False);
        targets = XInternAtom(d, "TARGETS", False);
        property = XInternAtom(d, "TERMINATOR_PASTE", False);
    }
    void copy(const std::string& text) {
        owned = text;
        XSetSelectionOwner(display, clipboard, window, CurrentTime);
        XFlush(display);
    }
    void requestPaste() {
        XConvertSelection(display, clipboard, utf8, property, window, CurrentTime);
        XFlush(display);
    }
    // Returns true when the event belongs to the clipboard handler.
    bool handle(const XEvent& event, std::string& pasted) {
        if (event.type == SelectionRequest) {
            const XSelectionRequestEvent& req = event.xselectionrequest;
            if (req.selection != clipboard) return false;
            XEvent reply{};
            reply.xselection.type = SelectionNotify;
            reply.xselection.display = display;
            reply.xselection.requestor = req.requestor;
            reply.xselection.selection = req.selection;
            reply.xselection.target = req.target;
            reply.xselection.time = req.time;
            reply.xselection.property = None;
            Atom dest = req.property == None ? req.target : req.property;
            if (req.target == targets) {
                Atom supported[] = {targets, utf8, XA_STRING};
                XChangeProperty(display, req.requestor, dest, XA_ATOM, 32,
                                PropModeReplace, reinterpret_cast<unsigned char*>(supported), 3);
                reply.xselection.property = dest;
            } else if (req.target == utf8 || req.target == XA_STRING) {
                XChangeProperty(display, req.requestor, dest, req.target, 8,
                                PropModeReplace, reinterpret_cast<const unsigned char*>(owned.data()),
                                static_cast<int>(owned.size()));
                reply.xselection.property = dest;
            }
            XSendEvent(display, req.requestor, False, 0, &reply);
            XFlush(display);
            return true;
        }
        if (event.type == SelectionNotify && event.xselection.selection == clipboard) {
            if (event.xselection.property == None) return true;
            Atom actual; int format; unsigned long count, remaining;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(display, window, property, 0, 16384, True,
                                   AnyPropertyType, &actual, &format, &count,
                                   &remaining, &data) == Success) {
                if (data && format == 8) pasted.assign(reinterpret_cast<char*>(data), count);
                if (data) XFree(data);
            }
            return true;
        }
        return false;
    }
};
#endif
