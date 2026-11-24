// chams_pose.h — Minecraft's biped model as data, and the pose maths that
// animates it. Pure arithmetic: no JNI, no GL, no ImGui. That is deliberate —
// this is the part most likely to be wrong, and keeping it dependency-free is
// what makes it testable outside the game.
//
// Coordinate conventions
// ----------------------
// MC authors the biped in MODEL UNITS (1/16 block) with Y pointing DOWN from an
// origin 1.501 blocks above the feet. LivingEntityRenderer then applies
// translate(0, -1.501, 0) followed by scale(-1, -1, 1), so a model-space point
// reaches entity-local space as:
//
//     local_x = -(model_x) / 16
//     local_y =  1.501 - (model_y) / 16
//     local_z =  (model_z) / 16
//
// Entity-local space here means: blocks, feet at y = 0, centred on x/z, and the
// body's own yaw not yet applied. block_of_*() below are the only place those
// three lines exist.
//
// A part's cuboid coordinates are expressed RELATIVE TO ITS PIVOT, so the
// absolute model-space extent is pivot + cuboid. That reading was cross-checked
// arithmetically against the independently-derived table in the PlayerESP
// (esp.cpp WIRE_PARTS): every one of the six parts agrees to the last decimal.
// See kParts below for the worked check.
//
// OPEN QUESTION, deliberately recorded rather than guessed: the sign convention
// for a part's Z pivot offset. Every stock biped part has pivot z = 0 and a
// z-symmetric box, so it cannot matter for vanilla geometry — but a live
// ModelPart reporting a nonzero z origin would expose it. The debug logger
// dumps live pivots so this can be settled from a real session instead of from
// memory.

#pragma once

namespace vanta::modules::chams::pose {

// ─────────────────────────────────────────────────────────────────────────────
// Parts, in the order every array in this module uses.
enum Part : int {
    kHead = 0,
    kBody = 1,
    kRightArm = 2,
    kLeftArm = 3,
    kRightLeg = 4,
    kLeftLeg = 5,
    kPartCount = 6,
};

// A part's authored geometry, in MODEL UNITS.
struct part_def {
    float pivot[3];      // rotation origin, model units
    float cuboid_min[3]; // box corner relative to the pivot
    float cuboid_max[3];
};

// The stock BipedEntityModel / PlayerEntityModel geometry.
//
// Worked cross-check against esp.cpp's WIRE_PARTS (which was derived
// independently, in blocks):
//   head     pivot(0,0,0)     box x[-4,4]  y[-8,0]   -> block x[-0.25,0.25]  y[1.501,2.001]   ESP: [-0.25,0.25] [1.50,2.00]
//   body     pivot(0,0,0)     box x[-4,4]  y[0,12]   -> block x[-0.25,0.25]  y[0.751,1.501]   ESP: [-0.25,0.25] [0.75,1.50]
//   rightArm pivot(-5,2,0)    box x[-3,1]            -> model x[-8,-4] -> block x[0.25,0.50]  ESP: [0.25,0.50]
//   rightLeg pivot(-1.9,12,0) box x[-2,2]            -> model x[-3.9,0.1] -> block x[-0.00625,0.24375]  ESP: [-0.00625,0.24375]
// The ESP's table rounds 1.501 to 1.50; that 1 mm is below a pixel at any
// range and is kept here at full precision.
extern const part_def kParts[kPartCount];

// The overlay layer (hat, jacket, sleeves, pants) is the same cuboid inflated
// uniformly. MC uses Dilation(0.5) on the hat and Dilation(0.25) on the other
// five. Inflation is in model units and applies to all six faces.
extern const float kOverlayDilation[kPartCount];

// Slim ("Alex") arms are 3 model units wide instead of 4, and the unit comes
// off the OUTER edge only — the shoulder pivot stays at +/-5. Verified in the
// PlayerEntityModel bytecode note recorded in esp.cpp: both variants build the
// arm at offset (-1,-2,-2) with pivot (+/-5, 2, 0); only the X size differs.
void apply_slim_arms(part_def parts[kPartCount]);

// ─────────────────────────────────────────────────────────────────────────────
// Everything the pose depends on, read from the entity once per frame.
struct pose_input {
    float body_yaw_deg    = 0.f;   // interpolated bodyYaw
    float head_pitch_deg  = 0.f;   // interpolated pitch
    float head_yaw_rel_deg= 0.f;   // headYaw - bodyYaw, wrapped to [-180,180]
    float limb_swing      = 0.f;   // LimbAnimator progress
    float limb_amplitude  = 0.f;   // LimbAnimator amplitude, clamped 0..1 by us
    float age_ticks       = 0.f;   // entity age + tick progress, for the idle sway
    float swing_progress  = 0.f;   // getHandSwingProgress, 0 = not swinging
    int   swinging_arm    = -1;    // kRightArm / kLeftArm, or -1
    bool  sneaking        = false;

    /* @the whole body tipped over: gliding, swimming, crawling.

       Vanilla does NOT do this with part angles. LivingEntityRenderer rotates
       the ENTIRE entity about X before the model is ever posed — about ninety
       degrees, so a gliding player lies flat. A pose built only from part angles
       therefore draws them standing upright while the game draws them
       horizontal, which is a figure-length error and reads as the outline
       having come off the player altogether.

       Elytra and swimming are the SAME rotation with different weights, which
       is why one field carries both:

           swimming   w = getLeaningPitch, target = in water ? 90 + pitch : 90
           gliding    w = clamp((glidingTicks + tickProgress)^2 / 100, 0, 1),
                      target = 90 + pitch

       Expressed in RADIANS and in the frame the caller applies it in, because
       the sign of a root rotation depends on that frame and nothing good comes
       of leaving that to be rediscovered. */
    float lean_rad        = 0.f;

    /* The swim POSE additionally shifts the body down and forward. Distinct
       from lean_rad being non-zero: crawling leans without this. */
    bool  swim_pose       = false;
    bool  main_hand_item  = false;
    bool  off_hand_item   = false;
    int   main_arm        = kRightArm;
    float scale           = 0.9375f; // MC draws the biped at a fixed 0.9375
};

// A part's rotation, about its own pivot, in radians. MC applies them in the
// order Rx then Ry then Rz (ModelPart.rotate).
struct part_pose {
    float pitch = 0.f;   // about X
    float yaw   = 0.f;   // about Y
    float roll  = 0.f;   // about Z
};

// Vanilla BipedEntityModel.setAngles, reimplemented: walk swing, idle arm sway,
// head look, sneak lean, held-item arm tilt and attack swing. Writes one
// part_pose per part.
//
// This is a REPLICATION of MC's formula, not a reading of MC's state. When the
// live ModelPart objects are reachable their angles should be preferred — they
// are authoritative and pick up modded models for free. This exists as the
// fallback for the versions where the live route is unavailable, and as the
// reference the live route is checked against in the debug log.
//
// `parts` is IN/OUT, and that is not decoration. Vanilla's attack swing does not
// only set angles: it rewrites the two arms' PIVOTS, walking both shoulders
// round the torso twist. A signature that returned angles alone could not say
// that, and the missing shoulder movement is what tore the swinging arm out of
// anything built from this pose. Pass the same array you gave apply_slim_arms —
// only the arm pivots are ever written, so a slim adjustment survives it.
void build_pose(const pose_input& in, part_def parts[kPartCount], part_pose out[kPartCount]);

// ─────────────────────────────────────────────────────────────────────────────
// Model units -> entity-local blocks. The only place the transform lives.
float block_of_x(float model_x);
float block_of_y(float model_y);
float block_of_z(float model_z);

// The eight corners of one part's cuboid, in entity-local blocks, with the
// part's own rotation applied about its pivot. Corner index is (xi*4+yi*2+zi),
// matching the face and edge tables in chams_geom.h.
//
// `dilation` inflates the box in model units (0 for the base layer).
void part_corners(const part_def& def, const part_pose& pose, float dilation,
                  float scale, float out_xyz[8][3]);

} // namespace vanta::modules::chams::pose
