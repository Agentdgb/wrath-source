#include "modules/combat/aimassist/simple/AimModel.h"
#include "modules/combat/aimassist/simple/MouseSensitivity.h"
#include <iostream>
#include <stdexcept>
using namespace readableaim;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        require(Settings{}.mode == AimMode::Simple, "Simple must remain the default");
        require(wrapDegrees(180) == -180 && wrapDegrees(-181) == 179, "Angular wrap differs from Minecraft");
        Settings settings;
        settings.speed1 = std::numeric_limits<double>::quiet_NaN();
        settings.speed2 = 999;
        settings.distance = -4;
        settings.sanitize();
        require(settings.speed1 == 20 && settings.speed2 == 100 && settings.distance == 1,
                "Invalid settings are not repaired");
        settings = {};
        Bounds box{{2.7, 0, -.3}, {3.3, 1.8, .3}};
        Point eye{0, 1.62, 0};
        for (double height : {0.2, 0.9, 1.7}) {
            auto camera = anglesTo(eye, {3, height, .2});
            auto desired = anglesTo(eye, aimPoint(box, eye, camera, settings, {.5, .65, .5}));
            const auto centered = anglesTo(eye, {3, height, 0});
            require(std::abs(wrapDegrees(desired.yaw - centered.yaw)) <
                        std::abs(wrapDegrees(camera.yaw - centered.yaw)),
                    "Tracking does not pull gently toward the hitbox center");
            require(std::abs(desired.pitch - camera.pitch) < .2,
                    "Center attraction changes the selected height");
        }
        settings.position = AimPosition::Random;
        auto random = aimPoint(box, eye, {}, settings, {.2, .8, .7});
        require(random.x > box.minimum.x && random.x < box.maximum.x && std::abs(random.y - 1.44) < 1e-6,
                "Random aim point is not inside the body");
        for (Point velocity : {Point{2, 3, -4}, Point{-9, -8, 7}, Point{}})
            for (AimPosition position : {AimPosition::Center, AimPosition::Random}) {
                settings.position = position;
                settings.prediction = .5;
                const auto point = constrainedAimPoint(box, velocity, eye, {}, settings, {.2, .8, .7});
                require(point.x > box.minimum.x && point.x < box.maximum.x &&
                        point.y > box.minimum.y && point.y < box.maximum.y &&
                        point.z > box.minimum.z && point.z < box.maximum.z,
                        "Prediction moves the aim point outside the real hitbox");
            }
        {
            AimEngine a, b;
            Settings options;
            const Angles first{10.031f, 0}, second{100.094f, 0};
            const auto x = a.update(first, {first.yaw+20, 0}, options, 1./144, false);
            const auto y = b.update(second, {second.yaw+20, 0}, options, 1./144, false);
            require(std::abs((x.camera.yaw-first.yaw)-(y.camera.yaw-second.yaw)) < .00002,
                    "Simple correction depends on absolute orientation");
            options.vertical = true;
            AimEngine stable;
            Angles view{10.031f, 0};
            for (int frame = 0; frame < 200; ++frame) {
                const auto result = stable.update(view, {view.yaw+.005f, 30}, options, 1./144, false);
                require(result.camera.yaw == view.yaw, "Pitch correction adds horizontal micro-jitter");
                view = result.camera;
            }
        }
        {
            Settings slowFine, fastFine;
            slowFine.speed2 = 1;
            fastFine.speed2 = 100;
            AimEngine slow, fast;
            require(slow.update({}, {3, 0}, slowFine, 1. / 60, false).camera.yaw <
                        fast.update({}, {3, 0}, fastFine, 1. / 60, false).camera.yaw,
                    "Fine tracking does not affect small corrections");
            slow.reset();
            fast.reset();
            require(slow.update({}, {30, 0}, slowFine, 1. / 60, false).camera.yaw ==
                        fast.update({}, {30, 0}, fastFine, 1. / 60, false).camera.yaw,
                    "Fine tracking changes large corrections");
        }
        {
            Settings options;
            AimEngine smooth;
            const auto first = smooth.update({}, {20, 0}, options, 1. / 60, false).camera;
            const auto next = smooth.update(first, {first.yaw + 20, 0}, options, 1. / 60, false).camera;
            require(next.yaw - first.yaw > first.yaw, "Tracking does not ease in");
            const auto reversed = smooth.update(next, {next.yaw - 20, 0}, options, 1. / 60, false).camera;
            require(reversed.yaw < next.yaw, "Tracking carries motion in the old direction");
        }
        {
            Settings directOptions, smoothOptions;
            directOptions.smoothing = 0;
            smoothOptions.smoothing = 80;
            AimEngine direct, smooth;
            require(direct.update({}, {20, 0}, directOptions, 1. / 60, false).camera.yaw >
                        smooth.update({}, {20, 0}, smoothOptions, 1. / 60, false).camera.yaw,
                    "Smoothing does not soften acquisition");
            Settings options;
            options.smoothing = 0;
            options.trackingStyle = TrackingStyle::Soft;
            AimEngine soft;
            const float softStep = soft.update({}, {20, 0}, options, 1. / 60, false).camera.yaw;
            options.trackingStyle = TrackingStyle::Balanced;
            AimEngine balanced;
            const float balancedStep = balanced.update({}, {20, 0}, options, 1. / 60, false).camera.yaw;
            options.trackingStyle = TrackingStyle::Strong;
            AimEngine strong;
            const float strongStep = strong.update({}, {20, 0}, options, 1. / 60, false).camera.yaw;
            require(softStep < balancedStep && balancedStep < strongStep, "Tracking presets have no distinct strength");
        }
        for (int fps : {30, 60, 144, 240}) {
            AimEngine tracking;
            Settings options;
            Angles view{};
            double accumulatedError = 0;
            for (int frame = 0; frame < fps * 4; ++frame) {
                const float targetYaw = static_cast<float>(12 * std::sin(frame * 2.0 / fps));
                view = tracking.update(view, {targetYaw, 0}, options, 1. / fps, false).camera;
                if (frame >= fps) accumulatedError += std::abs(targetYaw - view.yaw);
            }
            require(accumulatedError / (fps * 3) < 2.5, "Smooth tracking lags a moving target excessively");
        }
        auto future = predict(box, {.1, .2, -.1}, .1);
        require(std::abs(future.minimum.x - 2.9) < 1e-6 && std::abs(future.minimum.y - .4) < 1e-6,
                "Prediction does not use seconds and Minecraft ticks");
        require(angleError({0, 0}, {0, 40}, false) == 0 && angleError({0, 0}, {0, 40}, true) == 40,
                "Vertical aim does not control pitch FOV");
        for (AimMode mode : {AimMode::Simple})
            for (int fps : {30, 60, 144, 240, 1000}) {
                AimEngine engine;
                Settings options;
                options.mode = mode;
                options.vertical = true;
                Angles camera{};
                for (int frame = 0; frame < fps * 2; ++frame) {
                    auto result = engine.update(camera, {30, 20}, options, 1.0 / fps, false);
                    require(result.camera.yaw >= camera.yaw && result.camera.yaw <= 30 &&
                                result.camera.pitch >= camera.pitch && result.camera.pitch <= 20,
                            "Frame wrapper overshoots or moves away");
                    camera = result.camera;
                }
                require(camera.yaw > 27 && camera.pitch > 17, "Tracking does not converge at common FPS");
            }
        for (int fps : {30, 60, 144, 240, 1000}) {
            ManualReleaseFilter filter;
            ManualAimOverride latch;
            auto movement = filter.update(30, 0, -1.2, 0, false, 1. / fps);
            require(!latch.shouldSuspend(30, 0, movement.horizontal, 0, false, 1. / fps),
                    "An isolated small mouse batch releases tracking");
            bool released = false;
            filter.reset(); latch.reset();
            for (int frame = 0; frame < fps / 5; ++frame) {
                movement = filter.update(30, 0, -90. / fps, 0, false, 1. / fps);
                released |= latch.shouldSuspend(30, 0, movement.horizontal, 0, false, 1. / fps);
            }
            require(released, "Sustained deliberate turn does not release at common FPS");
            filter.reset(); latch.reset();
            movement = filter.update(30, 0, -15, 0, false, 1. / fps);
            require(latch.shouldSuspend(30, 0, movement.horizontal, 0, false, 1. / fps),
                    "A clear turn away must release immediately");
            movement = filter.update(30, 0, 2, 0, false, 1. / fps);
            require(!latch.shouldSuspend(30, 0, movement.horizontal, 0, false, 1. / fps),
                    "Turning toward the rival does not resume");
        }
        {
            AimEngine stable;
            Settings options;
            Angles view{};
            for (int frame = 0; frame < 240; ++frame) {
                if (frame % 30 == 10) view.yaw -= 1.2f;
                auto result = stable.update(view, {view.yaw + 30, 0}, options, 1. / 240, false);
                require(!result.manuallyReleased && result.camera.yaw > view.yaw,
                        "Intermittent mouse batches interrupt active tracking");
                view = result.camera;
            }
        }
        for (double sensitivity : {0., .25, .5, 1.}) {
            MouseSensitivityOutput output;
            const double step = mouseAngularStep(sensitivity);
            require(step > 0 && step <= .615, "Minecraft sensitivity scale is invalid");
            Angles view{};
            for (int i = 0; i < 300; ++i) {
                const auto next = output.apply(view, {view.yaw+.02f, view.pitch+.02f}, {10, 10}, true, sensitivity);
                require(next.yaw >= view.yaw && next.yaw <= 10 && next.pitch >= view.pitch && next.pitch <= 10,
                        "Sensitivity output overshoots or reverses");
                const double count = (next.yaw-view.yaw)/step;
                require(std::abs(count-std::round(count)) < .0001, "Correction is not in mouse-sized steps");
                view = next;
            }
            require(view.yaw > 5, "Fractional corrections stall at high sensitivity");
            for (int i = 0; i < 100; ++i) {
                const auto next = output.apply(view, {view.yaw+.001f, view.pitch}, {view.yaw+.001f, view.pitch}, true, sensitivity);
                require(next.yaw == view.yaw, "Sub-count target noise creates shake");
            }
        }
        AimEngine engine;
        settings = {};
        Angles camera = engine.update({}, {30, 0}, settings, 1. / 60, false).camera;
        camera.yaw -= 25;
        require(engine.update(camera, {30, 0}, settings, 1. / 60, false).manuallyReleased,
                "Turning away does not release the camera");
        for (int i = 0; i < 100; ++i)
            require(engine.update(camera, {30, 0}, settings, 1. / 60, false).camera.yaw == camera.yaw,
                    "Released camera pulls back without input");
        camera.yaw += 2;
        require(!engine.update(camera, {30, 0}, settings, 1. / 60, false).manuallyReleased,
                "Turning back does not resume assistance");
        engine.reset();
        auto outside = engine.update(camera, {80, 0}, settings, 1. / 60, true);
        require(outside.suspended && outside.camera.yaw == camera.yaw, "Retention outside range moves the camera");
        engine.reset();
        settings.vertical = true;
        for (int i = 0; i < 100; ++i)
            camera = engine.update(camera, {30, 20}, settings, 1. / 144, false).camera;
        require(camera.pitch > 10, "Vertical aim does not converge");
        std::cout << "PASS: native frame wrapper, target geometry, manual priority and FPS regression\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
