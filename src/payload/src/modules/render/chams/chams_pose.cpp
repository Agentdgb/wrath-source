#include "chams_pose.h"

#include <algorithm>
#include <cmath>

namespace vanta::modules::chams::pose {

namespace {
constexpr float kPi  = 3.14159265358979323846f;
constexpr float kDeg = kPi / 180.f;

// LivingEntityRenderer applies translate(0, -1.501, 0) before scale(-1,-1,1),
// so the model origin sits this far above the feet. The PlayerESP rounds it to
// 1.5; kept exact here because it costs nothing.
constexpr float kModelOriginY = 1.501f;
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Authored geometry, MODEL UNITS. cuboid_* are relative to the pivot.
//
//   head      ModelPartBuilder.cuboid(-4, -8, -4,  8,  8, 8), pivot (0, 0, 0)
//   body      ModelPartBuilder.cuboid(-4,  0, -2,  8, 12, 4), pivot (0, 0, 0)
//   rightArm  ModelPartBuilder.cuboid(-3, -2, -2,  4, 12, 4), pivot (-5, 2, 0)
//   leftArm   ModelPartBuilder.cuboid(-1, -2, -2,  4, 12, 4), pivot ( 5, 2, 0)
//   rightLeg  ModelPartBuilder.cuboid(-2,  0, -2,  4, 12, 4), pivot (-1.9, 12, 0)
//   leftLeg   ModelPartBuilder.cuboid(-2,  0, -2,  4, 12, 4), pivot ( 1.9, 12, 0)
//
// The arms are NOT mirror images in their local boxes: the right arm's box runs
// x[-3,+1] about its pivot and the left arm's x[-1,+3]. That asymmetry is what
// keeps both arms' inner faces flush with the body once the pivots are applied,
// and it is why apply_slim_arms() has to trim a different edge on each side.
const part_def kParts[kPartCount] = {
    /* head     */ { {  0.0f,  0.0f, 0.0f }, { -4.0f, -8.0f, -4.0f }, {  4.0f,  0.0f,  4.0f } },
    /* body     */ { {  0.0f,  0.0f, 0.0f }, { -4.0f,  0.0f, -2.0f }, {  4.0f, 12.0f,  2.0f } },
    /* rightArm */ { { -5.0f,  2.0f, 0.0f }, { -3.0f, -2.0f, -2.0f }, {  1.0f, 10.0f,  2.0f } },
    /* leftArm  */ { {  5.0f,  2.0f, 0.0f }, { -1.0f, -2.0f, -2.0f }, {  3.0f, 10.0f,  2.0f } },
    /* rightLeg */ { { -1.9f, 12.0f, 0.0f }, { -2.0f,  0.0f, -2.0f }, {  2.0f, 12.0f,  2.0f } },
    /* leftLeg  */ { {  1.9f, 12.0f, 0.0f }, { -2.0f,  0.0f, -2.0f }, {  2.0f, 12.0f,  2.0f } },
};

// Hat uses Dilation(0.5); jacket, both sleeves and both trouser legs use 0.25.
const float kOverlayDilation[kPartCount] = { 0.5f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f };

void apply_slim_arms(part_def parts[kPartCount])
{
    // 3 units wide instead of 4, taken off the OUTER edge only. The right arm's
    // outer edge is its -X face, the left arm's is its +X face.
    parts[kRightArm].cuboid_min[0] += 1.0f;   // -3 -> -2
    parts[kLeftArm ].cuboid_max[0] -= 1.0f;   //  3 ->  2
}

// ─────────────────────────────────────────────────────────────────────────────
float block_of_x(float model_x) { return -(model_x) / 16.f; }
float block_of_y(float model_y) { return kModelOriginY - (model_y) / 16.f; }
float block_of_z(float model_z) { return  (model_z) / 16.f; }

// ─────────────────────────────────────────────────────────────────────────────
void build_pose(const pose_input& in, part_def parts[kPartCount], part_pose out[kPartCount])
{
    for (int i = 0; i < kPartCount; ++i) out[i] = part_pose{};

    // Walk swing. MC writes cos(limbAngle*0.6662 + PI) for the right arm and
    // left leg; cos(x+PI) == -cos(x), so one cosine serves all four.
    const float cw = std::cos(in.limb_swing * 0.6662f);
    const float ld = std::clamp(in.limb_amplitude, 0.f, 1.f);

    // Idle sway: the arms breathe even when standing still.
    const float idle_pitch = std::sin(in.age_ticks * 0.067f) * 0.05f;
    const float idle_roll  = std::cos(in.age_ticks * 0.09f)  * 0.05f + 0.05f;

    const float head_pitch = in.head_pitch_deg * kDeg;

    out[kHead].pitch = head_pitch;
    /* Vanilla's ModelPart.yaw, verbatim. This used to be stored NEGATED, to
       cancel the transposed Ry in part_corners; that matrix is right-handed
       now, so the negation is gone with it. The scale(-1,-1,1) mirror does not
       owe one either way — it already lives inside block_of_x / block_of_y. */
    out[kHead].yaw   = in.head_yaw_rel_deg * kDeg;

    out[kRightArm].pitch = -cw * ld + idle_pitch;
    out[kLeftArm ].pitch =  cw * ld - idle_pitch;
    out[kRightArm].roll  =  idle_roll;
    out[kLeftArm ].roll  = -idle_roll;

    out[kRightLeg].pitch =  cw * 1.4f * ld;
    out[kLeftLeg ].pitch = -cw * 1.4f * ld;

    if (in.sneaking) {
        out[kBody].pitch     += 0.5f;
        out[kRightArm].pitch += 0.4f;
        out[kLeftArm ].pitch += 0.4f;
    }

    // ArmPose.ITEM: the holding arm tilts forward and stops tracking yaw.
    const int off_arm = (in.main_arm == kRightArm) ? kLeftArm : kRightArm;
    if (in.main_hand_item) {
        out[in.main_arm].pitch = out[in.main_arm].pitch * 0.5f - 0.31415927f;
        out[in.main_arm].yaw   = 0.f;
    }
    if (in.off_hand_item) {
        out[off_arm].pitch = out[off_arm].pitch * 0.5f - 0.31415927f;
        out[off_arm].yaw   = 0.f;
    }

    // ── Attack swing ────────────────────────────────────────────────────────
    //
    // This used to tilt the swinging arm and nothing else, and that is not what
    // vanilla does. setAngles TWISTS THE WHOLE TORSO for the duration of the
    // swing, walks BOTH shoulders round that twist, and yaws the swinging arm
    // at twice the torso's rate — the arm comes across the body, not straight
    // down it.
    //
    // Modelling only the tilt leaves the arm reaching one way while anything
    // built from this pose still has it hanging the other, which is exactly how
    // it presented: a player throwing a punch had the contour tear open around
    // the arm that was punching.
    //
    // The arms are SIBLINGS of the body in BipedEntityModel, not children of
    // it, so the torso's own rotation does not carry them. That is the whole
    // reason vanilla rewrites their pivots by hand, and why this has to as
    // well — which is also the answer to the open question in chams_pose.h
    // about a part's Z pivot: it is an ordinary model-space coordinate and
    // block_of_z() converts it like any other.
    if (in.swing_progress > 0.001f &&
        in.swinging_arm >= 0 && in.swinging_arm < kPartCount)
    {
        const float sw = in.swing_progress;

        // The torso twist. Reversed when it is the LEFT arm doing the swinging.
        float body_yaw = std::sin(std::sqrt(sw) * 2.f * kPi) * 0.2f;
        if (in.swinging_arm == kLeftArm) body_yaw = -body_yaw;

        out[kBody].yaw += body_yaw;

        // Both shoulders ride round the twist, on a circle of radius 5 about
        // the body's axis. At body_yaw = 0 these are exactly the rest pivots
        // (-5, 2, 0) and (5, 2, 0), so the swing starts and ends seamlessly.
        parts[kRightArm].pivot[0] = -std::cos(body_yaw) * 5.f;
        parts[kRightArm].pivot[2] =  std::sin(body_yaw) * 5.f;
        parts[kLeftArm ].pivot[0] =  std::cos(body_yaw) * 5.f;
        parts[kLeftArm ].pivot[2] = -std::sin(body_yaw) * 5.f;

        out[kRightArm].yaw   += body_yaw;
        out[kLeftArm ].yaw   += body_yaw;
        out[kLeftArm ].pitch += body_yaw;   // vanilla does this to the left arm only

        float f = 1.f - sw; f *= f; f *= f; f = 1.f - f;
        const float g = std::sin(f * kPi);
        const float h = std::sin(sw * kPi) * -(head_pitch - 0.7f) * 0.75f;

        out[in.swinging_arm].pitch -= g * 1.2f + h;
        out[in.swinging_arm].yaw   += body_yaw * 2.f;
        out[in.swinging_arm].roll  += std::sin(sw * kPi) * -0.4f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void part_corners(const part_def& def, const part_pose& pose, float dilation,
                  float scale, float out_xyz[8][3])
{
    const float cP = std::cos(pose.pitch), sP = std::sin(pose.pitch);
    const float cY = std::cos(pose.yaw),   sY = std::sin(pose.yaw);
    const float cR = std::cos(pose.roll),  sR = std::sin(pose.roll);

    const float lo[3] = { def.cuboid_min[0] - dilation,
                          def.cuboid_min[1] - dilation,
                          def.cuboid_min[2] - dilation };
    const float hi[3] = { def.cuboid_max[0] + dilation,
                          def.cuboid_max[1] + dilation,
                          def.cuboid_max[2] + dilation };

    int ci = 0;
    for (int xi = 0; xi < 2; ++xi)
    for (int yi = 0; yi < 2; ++yi)
    for (int zi = 0; zi < 2; ++zi) {
        // Corner offset from the pivot, in model units. The cuboid is already
        // expressed relative to the pivot, so this needs no subtraction.
        const float dx = xi ? hi[0] : lo[0];
        const float dy = yi ? hi[1] : lo[1];
        const float dz = zi ? hi[2] : lo[2];

        /* Rx then Ry then Rz, matching ModelPart.rotate.
         *
         * @the Ry line used to be TRANSPOSED, and it hid for a long time.
         *
         * It read `bx = ax*cY - az*sY … bz = ax*sY + az*cY`, which is a
         * rotation by MINUS yaw, sitting between an Rx and an Rz that are both
         * the ordinary right-handed form. One line out of three.
         *
         * It went unnoticed because the only yaw anyone ever pushed through it
         * was the head's, and build_pose stored THAT negated — two wrongs that
         * cancelled exactly. Nothing else in the pose carried a yaw at all
         * until the attack swing was completed, and the swinging arm carries
         * three times the torso twist, so it came out at minus three times
         * instead: an error of six times the twist, and a traced fist up to
         * 0.476 blocks adrift of a limb 0.234 wide.
         *
         * Verified: swapping this line and dropping that negation together
         * leave the head bit-identical at every angle, while the arm moves.
         * They are one change and must not be separated — the flip alone puts
         * a 70-degree head look 0.227 blocks out. */
        const float ax = dx,                ay = dy * cP - dz * sP, az = dy * sP + dz * cP;
        const float bx = ax * cY + az * sY, by = ay,               bz = -ax * sY + az * cY;
        const float ex = bx * cR - by * sR, ey = bx * sR + by * cR, ez = bz;

        // Back into absolute model space, then into entity-local blocks. The
        // pivot itself is NOT rotated — it is the centre of rotation.
        const float mx = def.pivot[0] + ex;
        const float my = def.pivot[1] + ey;
        const float mz = def.pivot[2] + ez;

        // The model scale multiplies the WHOLE entity-local position, origin
        // offset included. LivingEntityRenderer pushes scale(-1,-1,1), then the
        // model scale, then translate(0,-1.501,0) — and a vertex receives the
        // last-pushed transform first, so the 1.501 is inside the scale:
        //
        //     y = scale * (1.501 - model_y/16)
        //
        // Scaling only the geometry and re-adding an unscaled offset would put
        // a baby (scale ~0.5) at the wrong height entirely. Cross-checked
        // against esp.cpp's xform(), which computes (pvy) * sc with pvy already
        // in the 1.5 - m/16 form.
        out_xyz[ci][0] = block_of_x(mx) * scale;
        out_xyz[ci][1] = block_of_y(my) * scale;
        out_xyz[ci][2] = block_of_z(mz) * scale;
        ++ci;
    }
}

} // namespace vanta::modules::chams::pose
