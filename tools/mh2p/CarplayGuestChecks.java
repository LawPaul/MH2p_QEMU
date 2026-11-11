// SPDX-License-Identifier: GPL-2.0-or-later
import de.audi.app.terminalmode.keyevents.TouchEvent;
import de.audi.app.terminalmode.keyevents.ITerminalModeDSIKeyEventsController;
import de.audi.app.terminalmode.keyevents.TMVirtualButtonListener;
import de.audi.app.terminalmode.dsi.carplay.CarplayDSILifecycleController;
import de.audi.app.terminalmode.SmartphoneType;
import de.audi.app.terminalmode.device.ActiveDeviceState;
import de.audi.app.terminalmode.device.TMDevice;
import de.audi.app.terminalmode.device.TMDeviceID;
import de.audi.app.terminalmode.IContext;
import de.audi.app.terminalmode.config.ITerminalModeConfiguration;
import de.audi.app.terminalmode.smartphone.carplay.CarPlayDSIManager;
import de.audi.app.terminalmode.smartphone.carplay.CarplaySmartphoneProperties;
import de.audi.app.terminalmode.smartphone.IDSISmartphoneManagerListener;
import de.audi.app.terminalmode.statemachine.IStateHandler;
import de.audi.atip.utils.reactive.properties.PropertyFactory;
import de.audi.atip.log.LogChannel;
import de.esolutions.fw.util.commons.job.DispatcherBase;
import java.lang.reflect.Array;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.HashMap;
import java.util.Map;
import mh2p.lab.CarplayContactSlots;

/** Java 1.4-compatible smoke checks for the original QNX JVM, without HMI startup. */
public class CarplayGuestChecks {
    private static class ManagerFixture extends CarPlayDSIManager {
        ManagerFixture(IContext context, CarplayDSILifecycleController lifecycle,
                       IStateHandler state, CarplaySmartphoneProperties properties,
                       ITerminalModeConfiguration configuration,
                       IDSISmartphoneManagerListener listener) {
            super(context, lifecycle, state, properties, null, null,
                  null, null, configuration, null);
            smartphoneManagerListener = listener;
        }

        void markActive() {
            require(!active, "Manager fixture already active");
            active = true;
        }

        boolean isMarkedActive() {
            return active;
        }
    }

    public static class DiagnosticLog extends LogChannel {
        public void log(int l, String s, Object a, Object b, Object c, Object d,
                        Object e, long f, long g, long h, int i, Throwable t) {}
        public void log(int l, int s, Object a, Object b, Object c, Object d,
                        Object e, long f, long g, long h, int i, Throwable t) {}
        public void logException(Throwable t) { throw new RuntimeException(t); }
    }

    private static Object fixture(String type, final Map responses) throws Exception {
        Class iface = Class.forName(type);
        return Proxy.newProxyInstance(iface.getClassLoader(), new Class[] {iface},
            new InvocationHandler() {
                public Object invoke(Object proxy, Method method, Object[] values) {
                    if (!responses.containsKey(method.getName())) {
                        throw new IllegalStateException("Unexpected fixture call: " + method);
                    }
                    return responses.get(method.getName());
                }
            });
    }

    private static class Capture implements InvocationHandler {
        private int index;
        private final int[][][] expected;

        Capture(String scenario) {
            expected = scenario.equals("disabled") ? new int[0][][] :
                scenario.equals("filter-sticky") ? new int[][][] {
                {{373,269,1},{639,269,1}},
                {{0,254,2},{666,284,2}},
                {{100,200,2}},
                {{373,269,2},{100,200,2}},
                {{100,200,1}},
                {{100,200,2}}
            } : scenario.equals("filter-restart") ? new int[][][] {
                {{373,269,1},{639,269,1}},
                {{0,254,2},{666,284,2}},
                {{100,200,1}},
                {{100,200,2}}
            } : new int[][][] {
            {{373,269,1},{639,269,1}},
            {{346,254,1},{666,284,1}},
            {{346,254,2},{666,284,1}},
            {{346,254,2},{682,299,1}},
            {{346,254,2},{682,299,2}},
            {{373,269,1},{639,269,1}},
            {{346,254,1},{666,284,1}},
            {{346,254,2},{666,284,2}},
            {{100,200,1}},
            {{100,200,2}}
            };
        }

        public Object invoke(Object proxy, Method method, Object[] values) throws Exception {
            require(method.getName().equals("postMultiTouchEvent"), "Unexpected DSI method");
            require(index < expected.length, "Too many original DSI frames");
            Object[] contacts = (Object[]) values[1];
            require(contacts.length == expected[index].length, "Original DSI array length");
            System.out.print("GUEST_DSI");
            for (int i = 0; i < contacts.length; i++) {
                Class type = contacts[i].getClass();
                String[] getters = {"getX", "getY", "getTouchEventType"};
                System.out.print(" ");
                for (int j = 0; j < getters.length; j++) {
                    int actual = ((Integer) type.getMethod(getters[j], new Class[0])
                        .invoke(contacts[i], new Object[0])).intValue();
                    require(actual == expected[index][i][j], "Original DSI contact differs");
                    System.out.print((j == 0 ? "" : ",") + actual);
                }
            }
            System.out.println();
            index++;
            return null;
        }
    }

    private static void exerciseOriginalLifecycle(String scenario) throws Exception {
        boolean filterRestart = scenario.equals("filter-restart");
        boolean filterSticky = scenario.equals("filter-sticky");
        boolean disabled = scenario.equals("disabled");
        DiagnosticLog log = new DiagnosticLog();
        log.setLogThreshold(0);
        Map logs = new HashMap();
        logs.put("hmi", log);
        logs.put("dsi", log);
        logs.put("main", log);
        Object logger = fixture("de.audi.app.terminalmode.ITerminalLogger", logs);
        Map options = new HashMap();
        options.put("isKnobDirectionInverted", Boolean.FALSE);
        options.put("hasTouchscreen", disabled ? Boolean.FALSE : Boolean.TRUE);
        if (!disabled) {
            options.put("getCarplayScreenOffsetX", new Integer(0));
            options.put("getCarplayScreenOffsetY", new Integer(0));
            options.put("getDSICarPlayWindowResolutionX", new Integer(1920));
            options.put("getDSICarPlayWindowResolutionY", new Integer(720));
        }
        Object configuration = fixture(
            "de.audi.app.terminalmode.config.ITerminalModeConfiguration", options);
        Class listenerClass = Class.forName(
            "de.audi.app.terminalmode.keyevents.TMTouchscreenVBListener");
        Constructor listenerConstructor = listenerClass.getConstructor(new Class[] {
            Class.forName("de.audi.app.terminalmode.ITerminalLogger"),
            Class.forName("de.audi.app.terminalmode.config.ITerminalModeConfiguration"),
            Class.forName("de.audi.atip.hmi.HMIService"),
            Class.forName("de.audi.app.terminalmode.device.ActiveDeviceStatePublisher")});
        Object listener = listenerConstructor.newInstance(
            new Object[] {logger, configuration, null, null});
        Class deviceConsumer = Class.forName(
            "de.audi.app.terminalmode.keyevents.TMTouchscreenVBListener$1");
        Constructor consumerConstructor = deviceConsumer.getDeclaredConstructor(
            new Class[] {listenerClass});
        consumerConstructor.setAccessible(true);
        Object consumer = consumerConstructor.newInstance(new Object[] {listener});
        Method accept = deviceConsumer.getDeclaredMethod(
            "accept", new Class[] {ActiveDeviceState.class});
        accept.setAccessible(true);
        TMDevice device = new TMDevice(new TMDeviceID(
            "mh2p-diagnostic-carplay-device", SmartphoneType.CARPLAY));
        accept.invoke(consumer, new Object[] {new ActiveDeviceState(device, true)});
        Map services = new HashMap();
        services.put("getLogger", logger);
        services.put("getConfiguration", configuration);
        services.put("getTMKeyEventController", listener);
        services.put("getServiceManager", fixture(
            "de.audi.app.terminalmode.osgi.IServiceManager", new HashMap()));
        services.put("getFramework", fixture("de.audi.atip.base.IFrameworkAccess", new HashMap()));
        services.put("getDispatcher", new DispatcherBase("guest-contact-fixture",
            null, null, null, null));
        Map diagnosis = new HashMap();
        diagnosis.put("addCommandProvider", null);
        services.put("getDiagnosisManager", fixture(
            "de.audi.app.terminalmode.diagnosis.IDiagnosisManager", diagnosis));
        Object context = fixture("de.audi.app.terminalmode.IContext", services);
        Class safe = Class.forName("de.audi.app.terminalmode.dsi.carplay.DSICarplaySafe");
        Capture capture = new Capture(scenario);
        Object dsi = Proxy.newProxyInstance(safe.getClassLoader(), new Class[] {safe}, capture);
        Constructor lifecycleConstructor = CarplayDSILifecycleController.class.getConstructor(
            new Class[] {
                Class.forName("de.audi.app.terminalmode.IContext"),
                Class.forName("de.audi.app.terminalmode.dsi.carplay.IDSICarplayTransferObjectFactory"),
                safe, Class.forName("de.audi.app.terminalmode.dsi.carplay."
                    + "CarplayDSILifecycleController$DSICarplayListenerImpl"),
                Class.forName("de.audi.app.terminalmode.dsi.carplay.DSICarplaySafeProxy"),
                Class.forName("de.audi.app.terminalmode.smartphone.ISmartphoneProperties"),
                Class.forName("de.audi.app.terminalmode.smartphone.ILastmodehandler"),
                Class.forName("de.audi.app.terminalmode.smartphone.carplay.CarplayBTRequestHandler")});
        CarplayDSILifecycleController lifecycle = (CarplayDSILifecycleController)
            lifecycleConstructor.newInstance(
                new Object[] {context, null, dsi, null, null, null, null, null});
        ITerminalModeDSIKeyEventsController keys = lifecycle.getKeyEventController();
        ((TMVirtualButtonListener) listener).setDSIKeyEventController(keys);
        Map stateResponses = new HashMap();
        stateResponses.put("getCurrentState", null);
        stateResponses.put("removeStateChangeActions", null);
        IStateHandler stateHandler = (IStateHandler) fixture(
            "de.audi.app.terminalmode.statemachine.IStateHandler", stateResponses);
        Map propertyResponses = new HashMap();
        propertyResponses.put("createProperty", null);
        CarplaySmartphoneProperties properties = new CarplaySmartphoneProperties(
            lifecycle, log, null, (PropertyFactory) fixture(
                "de.audi.atip.utils.reactive.properties.PropertyFactory", propertyResponses),
            null, null, null, stateHandler, null, null);
        final int[] notifications = {0};
        Class managerListener = Class.forName(
            "de.audi.app.terminalmode.smartphone.IDSISmartphoneManagerListener");
        Object stateListener = Proxy.newProxyInstance(
            managerListener.getClassLoader(), new Class[] {managerListener},
            new InvocationHandler() {
                public Object invoke(Object proxy, Method method, Object[] values) {
                    require(method.getName().equals("updateDSIState")
                            && values.length == 1 && values[0].equals(Boolean.FALSE),
                            "Unexpected manager teardown notification");
                    notifications[0]++;
                    return null;
                }
            });
        ManagerFixture manager = new ManagerFixture(
            (IContext) context, lifecycle, stateHandler, properties,
            (ITerminalModeConfiguration) configuration,
            (IDSISmartphoneManagerListener) stateListener);
        Class pointClass = Class.forName("de.audi.atip.datatypes.TouchPoint");
        Constructor point = pointClass.getConstructor(
            new Class[] {Integer.TYPE, Integer.TYPE, Integer.TYPE, Integer.TYPE});
        Method moved = listenerClass.getMethod("touchScreenMoved", new Class[] {
            Array.newInstance(pointClass, 0).getClass(), Integer.TYPE, Integer.TYPE, Integer.TYPE});
        int[][][] frames = filterSticky ? new int[][][] {
            {{373,269,0,4},{639,269,1,4}},
            {{-1,254,0,5},{666,284,1,5}},
            {{100,200,7,4}},
            {{373,269,0,3},{100,200,7,3}},
            {{100,200,7,4}},
            {{100,200,7,3}}
        } : filterRestart ? new int[][][] {
            {{373,269,0,4},{639,269,1,4}},
            {{-1,254,0,5},{666,284,1,5}},
            null,
            {{100,200,7,4}},
            {{100,200,7,3}}
        } : new int[][][] {
            {{373,269,0,4},{639,269,1,4}},
            {{346,254,0,5},{666,284,1,5}},
            {{346,254,0,3},{666,284,1,5}},
            {{682,299,1,5}},
            {{682,299,1,3}},
            {{373,269,0,4},{639,269,1,4}},
            {{346,254,0,5},{666,284,1,5}},
            null,
            {{100,200,7,4}},
            {{100,200,7,3}}
        };
        keys.updateTouchEvents(new TouchEvent[0], new int[0]);
        manager.deactivate();
        require(notifications[0] == 0, "Inactive manager unexpectedly changed state");
        for (int i = 0; i < frames.length; i++) {
            if (frames[i] == null) {
                manager.markActive();
                manager.deactivate();
                require(!manager.isMarkedActive(), "Original manager did not clear active flag");
                require(notifications[0] == 1, "Original active teardown notification missing");
                manager.deactivate();
                require(notifications[0] == 1, "Inactive repeat notified teardown twice");
                continue;
            }
            Object points = Array.newInstance(pointClass, frames[i].length);
            for (int j = 0; j < frames[i].length; j++) {
                int[] p = frames[i][j];
                Array.set(points, j, point.newInstance(new Object[] {
                    new Integer(p[0]), new Integer(p[1]), new Integer(p[2]), new Integer(p[3])}));
            }
            moved.invoke(listener, new Object[] {
                points, new Integer(frames[i].length), new Integer(0), new Integer(0)});
            if (i == 0) {
                manager.deactivate();
            }
        }
        keys.updateTouchEvents(new TouchEvent[0], new int[0]);
        require(capture.index == (disabled ? 0 : filterSticky ? 6 : filterRestart ? 4 : 10),
                "Missing original Java DSI frames");
        System.out.println(disabled ? "MH2P_CARPLAY_GUEST_TOUCHSCREEN_DISABLED_PASS"
                           : filterSticky ? "MH2P_CARPLAY_GUEST_FILTER_STICKY_PASS"
                           : filterRestart ? "MH2P_CARPLAY_GUEST_FILTER_RESTART_PASS"
                           : "MH2P_CARPLAY_GUEST_LIFECYCLE_PASS");
    }

    private static void require(boolean value, String message) {
        if (!value) {
            throw new IllegalStateException(message);
        }
    }

    public static void main(String[] args) throws Exception {
        Object owner = new Object();
        TouchEvent first = new TouchEvent(true, 100, 200, true, 0);
        TouchEvent second = new TouchEvent(true, 700, 500, true, 1);
        TouchEvent[] events = CarplayContactSlots.normalize(
            owner, new TouchEvent[] {first, second});
        require(events.length == 2, "Two-contact press lost");
        first = new TouchEvent(false, 100, 200, true, 0);
        events = CarplayContactSlots.normalize(owner, new TouchEvent[] {first, second});
        require(events[0].getTouchState() == 1, "First contact not released");
        second = new TouchEvent(700, 500, 710, 510, true, 1);
        events = CarplayContactSlots.normalize(owner, new TouchEvent[] {second});
        require(events.length == 2 && events[1].getFingerId() == 1
                && events[1].getCurrentX() == 710, "Survivor slot changed");
        events = CarplayContactSlots.normalize(owner, new TouchEvent[0]);
        require(events.length == 2 && events[0].getTouchState() == 1
                && events[1].getTouchState() == 1, "Cancellation left contacts down");
        events = CarplayContactSlots.normalize(owner, new TouchEvent[] {
            new TouchEvent(true, 50, 60, true, 7)});
        require(events.length == 1 && events[0].getFingerId() == 7,
                "New gesture inherited stale slots");
        String[] classes = {
            "de.audi.app.terminalmode.keyevents.TMTouchscreenVBListener",
            "de.audi.app.terminalmode.dsi.carplay."
                + "CarplayDSILifecycleController$TerminalModeDSIKeyEventsController",
            "de.audi.app.terminalmode.smartphone.carplay.CarPlayDSIManager"
        };
        for (int i = 0; i < classes.length; i++) {
            require(Class.forName(classes[i]).getDeclaredMethods().length > 0,
                    "Modified original class has no methods");
        }
        require(args.length <= 1 && (args.length == 0 || args[0].startsWith("--")),
                "Unknown guest scenario");
        String scenario = args.length == 0 ? "lifecycle" : args[0].substring(2);
        require(scenario.equals("lifecycle")
                || scenario.equals("filter-restart") || scenario.equals("filter-sticky")
                || scenario.equals("disabled"),
                "Unknown guest scenario");
        exerciseOriginalLifecycle(scenario);
        System.out.println("MH2P_CARPLAY_GUEST_JAVA_CHECKS_PASS");
    }
}
