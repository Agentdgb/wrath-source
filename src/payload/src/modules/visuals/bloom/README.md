# Native world effects

WorldParticles and TargetESP share the native world pass. Atmosphere, DeathEffects, FallingPetals, SlowSwing and SwordReplace have been removed from the client, including hooks, UI, packet observation and embedded sky/petal resources. Offline sky image utilities remain independent.

WorldParticles defaults to 35% density. The selected families share one density budget. Geometry is bounded to 512 lines, 128 quads, 256 sprites and 128 trail sprites before TargetESP runs. Terrain uses a fixed 128-entry cache with 0.5-second expiry and at most four fresh height queries per frame; exhausted queries use the camera ground level. Reset clears the cache. Particle code reuses array references and no longer samples the complete player entity each frame.

Regression coverage exercises all five families together and separately at full density, zero density, geometry/query limits, terrain cache expiry/reset, disable/fade and TargetESP styles. Runtime checks cover SwingAnimation and Sodium/Chams visibility. Actual game FPS still needs to be measured on the target machine.
