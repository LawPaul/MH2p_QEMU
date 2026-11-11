// SPDX-License-Identifier: GPL-2.0-or-later
import java.lang.reflect.*;
import java.util.*;
import sun.misc.Unsafe;
import de.audi.atip.log.LogChannel;

public class CarplayJavaTrace {
    public static class DiagnosticLog extends LogChannel {
        public void log(int l, String s, Object a, Object b, Object c, Object d,
                        Object e, long f, long g, long h, int i, Throwable t) {}
        public void log(int l, int s, Object a, Object b, Object c, Object d,
                        Object e, long f, long g, long h, int i, Throwable t) {}
        public void logException(Throwable t) { throw new RuntimeException(t); }
    }

    static final String KEY = "de.audi.app.terminalmode.keyevents.";
    static final String CAR = "de.audi.app.terminalmode.dsi.carplay.";

    static void set(Object target, String name, Object value) throws Exception {
        for (Class<?> c = target.getClass(); c != null; c = c.getSuperclass()) {
            try {
                Field f = c.getDeclaredField(name);
                f.setAccessible(true);
                f.set(target, value);
                return;
            } catch (NoSuchFieldException e) {
                // Continue through the original class hierarchy.
            }
        }
        throw new NoSuchFieldException(name);
    }

    public static void main(String[] args) throws Exception {
        Field field = Unsafe.class.getDeclaredField("theUnsafe");
        field.setAccessible(true);
        Unsafe unsafe = (Unsafe) field.get(null);
        DiagnosticLog logger = new DiagnosticLog();
        logger.setLogThreshold(0);
        Class<?> listenerClass = Class.forName(KEY + "TMTouchscreenVBListener");
        Object listener = unsafe.allocateInstance(listenerClass);
        set(listener, "lastTouchEvents", new HashMap<Object, Object>());
        set(listener, "logger", logger);
        Class<?> pointClass = Class.forName("de.audi.atip.datatypes.TouchPoint");
        Constructor<?> point = pointClass.getConstructor(int.class, int.class,
                                                        int.class, int.class);
        Class<?> resultClass = Class.forName(KEY + "TouchPointFilter$TouchPointFilterResults");
        Constructor<?> result = resultClass.getDeclaredConstructor(
            Array.newInstance(pointClass, 0).getClass(), boolean.class);
        result.setAccessible(true);
        Method determine = listenerClass.getDeclaredMethod("determineTouchEvents", resultClass);
        determine.setAccessible(true);
        Class<?> safeClass = Class.forName(CAR + "DSICarplaySafe");
        Object capture = Proxy.newProxyInstance(safeClass.getClassLoader(),
            new Class<?>[] {safeClass}, (proxy, method, values) -> {
                if (!method.getName().equals("postMultiTouchEvent")) {
                    throw new IllegalStateException("Unexpected DSI call " + method);
                }
                Object[] contacts = (Object[]) values[1];
                System.out.print("DSI");
                for (Object event : contacts) {
                    Class<?> c = event.getClass();
                    System.out.print(" " + c.getMethod("getX").invoke(event) + ","
                        + c.getMethod("getY").invoke(event) + ","
                        + c.getMethod("getTouchEventType").invoke(event));
                }
                System.out.println();
                return null;
            });
        Object controller = unsafe.allocateInstance(Class.forName(CAR + "CarplayDSILifecycleController"));
        set(controller, "logger", logger);
        set(controller, "dsiCarplaySafe", capture);
        Class<?> keysClass = Class.forName(CAR +
            "CarplayDSILifecycleController$TerminalModeDSIKeyEventsController");
        Object keys = unsafe.allocateInstance(keysClass);
        set(keys, "this$0", controller);
        set(controller, "keyEventController", keys);
        Class<?> managerClass = Class.forName(
            "de.audi.app.terminalmode.smartphone.carplay.CarPlayDSIManager");
        Object manager = unsafe.allocateInstance(managerClass);
        set(manager, "logger", logger);
        set(manager, "carPlayDSILifecycleController", controller);
        Class<?> contextClass = Class.forName("de.audi.app.terminalmode.IContext");
        Object context = Proxy.newProxyInstance(contextClass.getClassLoader(),
            new Class<?>[] {contextClass}, (proxy, method, values) -> {
                if (!method.getName().equals("getTMKeyEventController")) {
                    throw new IllegalStateException("Unexpected context call " + method);
                }
                return listener;
            });
        set(manager, "context", context);
        Method update = keysClass.getDeclaredMethod("updateTouchEvents",
            Class.forName("[L" + KEY + "TouchEvent;"), int[].class);
        update.setAccessible(true);
        boolean cancellation = args.length == 1 && args[0].equals("--cancellation");
        boolean reconnect = args.length == 1 && args[0].equals("--reconnect");
        boolean empty = args.length == 1 && args[0].equals("--empty");
        if (args.length != 0 && !cancellation && !reconnect && !empty) {
            throw new IllegalArgumentException("Unknown lifecycle scenario");
        }
        if (empty) {
            update.invoke(keys, Array.newInstance(Class.forName(KEY + "TouchEvent"), 0),
                          new int[0]);
        }
        int[][][] frames = reconnect ? new int[][][] {
            {{373,269,0,4},{639,269,1,4}},
            {{346,254,0,5},{666,284,1,5}},
            null,
            {{100,200,7,4}},
            {{100,200,7,3}}
        } : cancellation ? new int[][][] {
            {{373,269,0,4},{639,269,1,4}},
            {{346,254,0,5},{666,284,1,5}},
            null,
            {{346,254,0,3},{666,284,1,3}},
            {{100,200,7,4}},
            {{100,200,7,3}}
        } : new int[][][] {
            {{373,269,0,4},{639,269,1,4}},
            {{346,254,0,5},{666,284,1,5}},
            {{346,254,0,3},{666,284,1,5}},
            {{682,299,1,5}},
            {{682,299,1,3}}
        };
        for (int[][] frame : frames) {
            if (frame == null) {
                managerClass.getMethod("deactivate").invoke(manager);
                managerClass.getMethod("deactivate").invoke(manager);
                continue;
            }
            Object points = Array.newInstance(pointClass, frame.length);
            int[] ids = new int[frame.length];
            for (int i = 0; i < frame.length; i++) {
                int[] p = frame[i];
                Array.set(points, i, point.newInstance(p[0],p[1],p[2],p[3]));
                ids[i] = p[2];
            }
            Object events = determine.invoke(listener, result.newInstance(points, false));
            System.out.print("HMI");
            for (Object e : (Object[]) events) {
                Class<?> c = e.getClass();
                System.out.print(" " + c.getMethod("getFingerId").invoke(e) + ":"
                    + c.getMethod("getCurrentX").invoke(e) + ","
                    + c.getMethod("getCurrentY").invoke(e) + ","
                    + c.getMethod("getTouchState").invoke(e));
            }
            System.out.println();
            update.invoke(keys, events, ids);
        }
        if (empty) {
            update.invoke(keys, Array.newInstance(Class.forName(KEY + "TouchEvent"), 0),
                          new int[0]);
            update.invoke(keys, Array.newInstance(Class.forName(KEY + "TouchEvent"), 0),
                          new int[0]);
        }
    }
}
