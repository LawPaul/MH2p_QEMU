// SPDX-License-Identifier: GPL-2.0-or-later
package mh2p.lab;

import java.util.ArrayList;
import java.util.Map;
import java.util.WeakHashMap;
import de.audi.app.terminalmode.keyevents.TouchEvent;
import de.audi.app.terminalmode.keyevents.ITerminalModeDSIKeyEventsController;
import de.audi.app.terminalmode.keyevents.TMVirtualButtonListener;
import de.audi.app.terminalmode.keyevents.TouchPointFilter;
import de.audi.app.terminalmode.dsi.carplay.CarplayDSILifecycleController;
import de.audi.atip.utils.generics.GSet;

/** Opt-in laboratory adapter at the CarPlay-only Java array boundary. */
public final class CarplayContactSlots {
    private static final Map states = new WeakHashMap();
    private static final Map resets = new WeakHashMap();
    private static final Map filters = new WeakHashMap();
    private static final Map filterResets = new WeakHashMap();

    private CarplayContactSlots() {}

    public static synchronized void cancel(CarplayDSILifecycleController lifecycle,
                                           TMVirtualButtonListener listener) {
        if (lifecycle == null || listener == null) {
            throw new IllegalArgumentException("CarPlay lifecycle and listener are required");
        }
        ITerminalModeDSIKeyEventsController owner = lifecycle.getKeyEventController();
        resets.put(listener, Boolean.TRUE);
        Object filter = filters.get(listener);
        if (filter != null) {
            filterResets.put(filter, Boolean.TRUE);
        }
        if (states.containsKey(owner)) {
            owner.updateTouchEvents(new TouchEvent[0], new int[0]);
        }
    }

    public static synchronized void prepareListener(Object listener, Map contacts,
                                                     TouchPointFilter filter) {
        filters.put(listener, filter);
        if (resets.remove(listener) != null) {
            contacts.clear();
        }
    }

    public static synchronized void prepareFilter(Object filter, GSet rejected) {
        if (filterResets.remove(filter) != null) {
            rejected.clear();
        }
    }

    public static synchronized TouchEvent[] normalize(Object owner, TouchEvent[] incoming) {
        if (owner == null || incoming == null) {
            throw new IllegalArgumentException("CarPlay contact owner and array are required");
        }
        for (int i = 0; i < incoming.length; i++) {
            TouchEvent event = incoming[i];
            if (event == null) {
                throw new IllegalArgumentException("Null CarPlay contact");
            }
            for (int j = 0; j < i; j++) {
                if (incoming[j].getFingerId() == event.getFingerId()) {
                    throw new IllegalArgumentException("Duplicate CarPlay finger ID");
                }
            }
        }
        ArrayList previous = (ArrayList) states.get(owner);
        ArrayList slots = previous == null
            ? new ArrayList() : new ArrayList(previous);
        for (int i = 0; i < incoming.length; i++) {
            TouchEvent event = incoming[i];
            int position = -1;
            for (int j = 0; j < slots.size(); j++) {
                if (((TouchEvent) slots.get(j)).getFingerId() == event.getFingerId()) {
                    position = j;
                    break;
                }
            }
            if (position < 0) {
                for (int j = 0; j < slots.size(); j++) {
                    TouchEvent old = (TouchEvent) slots.get(j);
                    boolean included = false;
                    for (int k = 0; k < incoming.length; k++) {
                        TouchEvent candidate = incoming[k];
                        if (candidate.getFingerId() == old.getFingerId()) {
                            included = true;
                            break;
                        }
                    }
                    if (old.getTouchState() == 1 && !included) {
                        position = j;
                        break;
                    }
                }
            }
            if (position < 0) {
                if (slots.size() == 4) {
                    throw new IllegalArgumentException("More than four CarPlay contact slots");
                }
                slots.add(event);
            } else {
                slots.set(position, event);
            }
        }
        boolean active = false;
        if (incoming.length == 0) {
            for (int i = 0; i < slots.size(); i++) {
                TouchEvent old = (TouchEvent) slots.get(i);
                slots.set(i, new TouchEvent(false, old.getCurrentX(), old.getCurrentY(),
                                           old.isTouchScreen(), old.getFingerId()));
            }
        }
        for (int i = 0; i < slots.size(); i++) {
            TouchEvent event = (TouchEvent) slots.get(i);
            if (event.getTouchState() != 1) {
                active = true;
            }
        }
        if (active) {
            states.put(owner, slots);
        } else {
            states.remove(owner);
        }
        return (TouchEvent[]) slots.toArray(new TouchEvent[slots.size()]);
    }
}
