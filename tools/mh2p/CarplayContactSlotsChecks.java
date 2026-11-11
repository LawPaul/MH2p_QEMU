// SPDX-License-Identifier: GPL-2.0-or-later
import de.audi.app.terminalmode.keyevents.TouchEvent;
import mh2p.lab.CarplayContactSlots;

/** Host-only assertions against the guest-compatible helper and original TouchEvent. */
public class CarplayContactSlotsChecks {
    private static int checks;

    private static TouchEvent point(int id, boolean down) {
        return new TouchEvent(down, 100 + id, 200 + id, true, id);
    }

    private static TouchEvent[] frame(int... ids) {
        TouchEvent[] result = new TouchEvent[ids.length];
        for (int i = 0; i < ids.length; i++) {
            result[i] = point(ids[i], true);
        }
        return result;
    }

    private static void expect(Object owner, TouchEvent[] incoming, int... slots) {
        TouchEvent[] result = CarplayContactSlots.normalize(owner, incoming);
        if (result.length != slots.length / 2) {
            throw new AssertionError("Unexpected contact slot count");
        }
        for (int i = 0; i < result.length; i++) {
            TouchEvent event = result[i];
            if (event.getFingerId() != slots[i * 2]
                    || (event.getTouchState() != 1) != (slots[i * 2 + 1] == 1)
                    || event.getCurrentX() != 100 + event.getFingerId()
                    || event.getCurrentY() != 200 + event.getFingerId()) {
                throw new AssertionError("Unexpected slot " + i);
            }
        }
        checks++;
    }

    private static void reject(Object owner, TouchEvent[] incoming) {
        try {
            CarplayContactSlots.normalize(owner, incoming);
        } catch (IllegalArgumentException expected) {
            checks++;
            return;
        }
        throw new AssertionError("Invalid input accepted");
    }

    public static void main(String[] args) {
        Object owner = new Object();
        Object other = new Object();
        expect(owner, frame(0, 1, 2, 3), 0, 1, 1, 1, 2, 1, 3, 1);
        expect(owner, new TouchEvent[] {point(0, true), point(1, false),
               point(2, true), point(3, true)}, 0, 1, 1, 0, 2, 1, 3, 1);
        expect(owner, frame(3, 2, 0), 0, 1, 1, 0, 2, 1, 3, 1);
        expect(owner, frame(0, 2, 3, 4), 0, 1, 4, 1, 2, 1, 3, 1);
        expect(other, frame(7), 7, 1);
        reject(owner, new TouchEvent[] {point(0, true), null});
        reject(owner, frame(0, 0));
        reject(owner, frame(0, 2, 3, 4, 5));
        reject(null, frame(0));
        reject(owner, null);
        expect(owner, frame(3, 0, 4, 2), 0, 1, 4, 1, 2, 1, 3, 1);
        expect(owner, new TouchEvent[0], 0, 0, 4, 0, 2, 0, 3, 0);
        expect(owner, frame(9), 9, 1);
        expect(owner, new TouchEvent[] {point(9, false)}, 9, 0);
        expect(owner, frame(12, 13), 12, 1, 13, 1);
        expect(owner, new TouchEvent[] {point(12, false), point(13, true)},
               12, 0, 13, 1);
        expect(owner, frame(13), 12, 0, 13, 1);
        expect(owner, new TouchEvent[] {point(13, false)}, 12, 0, 13, 0);
        expect(owner, new TouchEvent[0]);
        expect(other, new TouchEvent[] {point(7, false)}, 7, 0);
        System.out.println("{\"helper_checks_passed\":" + checks + "}");
    }
}
