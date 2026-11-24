// Clicks between frames: pressed from inside player 1's physics update, split the way Click Between Frames v1.5.0 splits
// it (theyareonit/Click-Between-Frames, MIT), as the Frame Window Counter's FWCPlayerStep does (v1.64, v1.137):
//   - everything happens in player 1's update. For each click: both players move up to its moment (an update with that
//     part of the step, a collision check that can't kill, rotation, the collision log cleared), then the click is
//     pressed; the rest of the step follows.
//   - a player is only moved in pieces when it's on the ground, touching an orb, dashing, or a ship / wave / UFO /
//     swing. Otherwise it does its one whole update after the click (CBF's "buffering").
#include "stepper.hpp"

#include <Geode/modify/PlayerObject.hpp>

namespace xdstep {

SubState& sub() {
    static SubState s;
    return s;
}

std::string cbfConflictText() {
    if (auto m = Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
        if (!m->getSettingValue<bool>("soft-toggle")) return "Click Between Frames is on";
        if (m->getSettingValue<bool>("physics-bypass")) return "CBF's Physics Bypass is on";
    }
    if (Loader::get()->isModLoaded("zmx.cbf-lite")) return "CBF Lite is loaded";
    if (Loader::get()->isModLoaded("chizz.superb-input-precision")) return "Superb Input Precision is loaded";
    return {};
}

bool cbfActive() { return !cbfConflictText().empty(); }

void arm(input const& e) {
    auto& a = sub().armed;
    auto it = std::upper_bound(a.begin(), a.end(), e.sub, [](long long s, input const& x) { return s < x.sub; });
    a.insert(it, e);
}

void clearArmed() { sub().armed.clear(); }

void pressNow(GJBaseGameLayer* gl, input const& e) {
    if (!gl) return;
    bool player2 = e.player2;
    if (Macro::flipControls()) player2 = !player2;
    auto& s = sub();
    ++s.pressing;
    gl->handleButton(e.down, e.button, player2);
    --s.pressing;
}

void flushArmed(GJBaseGameLayer* gl) {
    auto armed = std::move(sub().armed);
    sub().armed.clear();
    for (auto const& e : armed) pressNow(gl, e);
}

namespace {
void resetCollisionLog(PlayerObject* p) {
    if (p->m_collisionLogTop) p->m_collisionLogTop->removeAllObjects();
    if (p->m_collisionLogBottom) p->m_collisionLogBottom->removeAllObjects();
    if (p->m_collisionLogLeft) p->m_collisionLogLeft->removeAllObjects();
    if (p->m_collisionLogRight) p->m_collisionLogRight->removeAllObjects();
    p->m_lastCollisionLeft = -1;
    p->m_lastCollisionRight = -1;
    p->m_lastCollisionBottom = -1;
    p->m_lastCollisionTop = -1;
}
} // namespace

} // namespace xdstep

class $modify(XDSubtickStep, PlayerObject) {
    void update(float dt) {
        auto& s = xdstep::sub();
        if (s.mid) return PlayerObject::update(dt); // a piece of a split update
        PlayLayer* pl = PlayLayer::get();
        if (pl && this == pl->m_player2 && s.p2Done) {
            s.p2Done = false; // already moved inside player 1's update
            return;
        }
        if (pl && this == pl->m_player1) {
            s.p2Done = s.p1Split = s.p2Split = false;
            if (!s.armed.empty() && !Global::get().creatingTrajectory) {
                this->splitUpdate(pl, dt);
                return;
            }
        }
        PlayerObject::update(dt);
    }

    // player 1's update with the armed clicks pressed inside it
    void splitUpdate(PlayLayer* pl, float dt) {
        auto& s = xdstep::sub();
        auto clicks = std::move(s.armed);
        s.armed.clear();

        PlayerObject* p2 = pl->m_player2;
        bool const dual = p2 && pl->m_gameState.m_isDualMode;
        auto movesInPieces = [](PlayerObject* o) {
            return o->m_isOnGround || (o->m_touchingRings && o->m_touchingRings->count() > 0) || o->m_isDashing
                || o->m_isDart || o->m_isBird || o->m_isShip || o->m_isSwing;
        };
        bool const p1Ground = m_isOnGround;
        bool const p2Ground = p2 && p2->m_isOnGround;
        s.p1Pos = this->getPosition();
        if (p2) s.p2Pos = p2->getPosition();
        s.p1Split = movesInPieces(this);
        s.p2Split = dual && movesInPieces(p2);
        ++s.splitUpdates;
        if (!s.p1Split && !s.p2Split) ++s.bufferedUpdates;

        auto piece = [&](PlayerObject* o, float pieceDt, bool first, bool startedOnGround) {
            o->update(pieceDt);
            // (CBF: fixes delayed inputs on platforms moving down)
            if (first && ((o->m_yVelocity < 0) != o->m_isUpsideDown)) o->m_isOnGround = startedOnGround;
            // (CBF: 0 for the step length, or moving platforms launch the player; the whole step on a slope)
            if (!o->m_isOnSlope || o->m_isDart) pl->checkCollisions(o, 0.f, true);
            else pl->checkCollisions(o, dt, true);
            o->updateRotation(pieceDt);
            xdstep::resetCollisionLog(o);
        };
        constexpr double kSmallest = std::numeric_limits<float>::min();
        double elapsed = 0.0;
        bool first = true;
        s.mid = true;
        for (auto const& e : clicks) {
            double const at = xdstep::fractionOf(e);
            float const pieceDt = static_cast<float>(dt * std::clamp(at - elapsed, kSmallest, 1.0));
            elapsed = at;
            if (s.p1Split) piece(this, pieceDt, first, p1Ground);
            if (s.p2Split) piece(p2, pieceDt, first, p2Ground);
            first = false;
            xdstep::pressNow(pl, e);
        }
        float const rest = static_cast<float>(dt * std::max(kSmallest, 1.0 - elapsed));
        s.rotDt = rest;
        this->update(s.p1Split ? rest : dt);
        if (dual) {
            p2->update(s.p2Split ? rest : dt);
            s.p2Done = true; // GD's own call for player 2 this step is skipped
        }
        s.mid = false;
    }

    // At the end of GD's step for each player: after a split, only the rotation of the last piece is left to do, and
    // the step's movement is measured from where the player started it.
    void updateRotation(float dt) {
        auto& s = xdstep::sub();
        if (!s.mid && (s.p1Split || s.p2Split)) {
            PlayLayer* pl = PlayLayer::get();
            if (pl && this == pl->m_player1 && s.p1Split) {
                s.p1Split = false;
                PlayerObject::updateRotation(s.rotDt);
                m_lastPosition = s.p1Pos;
                return;
            }
            if (pl && this == pl->m_player2 && s.p2Split) {
                s.p2Split = false;
                PlayerObject::updateRotation(s.rotDt);
                m_lastPosition = s.p2Pos;
                return;
            }
        }
        PlayerObject::updateRotation(dt);
    }
};
