# Action continuity and localized impacts

## Motion changes

- Two-bone IK uses one bend-plane normal for both segments. A deeply folded elbow or knee no longer flips its lower segment by 180 degrees when it passes perpendicular to the reach direction.
- Stance hand targets carry a blend weight. A missing target fades in or out continuously, including kneel/ground/get-up transitions. Procedural head look reduces with the stance's turning freedom.
- Arm blends choose a continuous quaternion arc near a half-turn. Requested shoulder/wrist speed is bounded at 30 rad/s and elbow speed at 36 rad/s. These bounds affect the planned pose; external forces still act on the physical body.
- Strikes use smooth acceleration and deceleration. The roundhouse counterbalance hand travels around the shoulder, with less combined hip/torso lean and an authored knee pole. Knife windups keep space around the shoulder and use smaller wrist turns.
- Kicking feet use the held-foot drive even when their planner still marks them planted. The drive has enough force for the chamber and extension, with more ankle damping.
- Combat actions prepare a lead-foot support step and turn. Mirroring includes that step, knee pole and knife ownership. The physical knife follows the selected hand, including limb-loss handling.
- An untargeted strike gets a forward target at an action-specific height. Outside guard it has a 0.18-second preparation. Explicit game targets retain their authored timing. Manually selected pose actions stay held until interrupted.
- Interruptions fade from the current weight, including during preparation, and cannot emit a strike/reload event after cancellation.

## Impact API

`HitInfo::force` controls injury severity, stun and reaction strength. `HitInfo::impulse_ns` independently controls transferred momentum in **N s**. Negative values choose defaults: bullet `5*sqrt(force)`, blunt `34*force`, blade `9*force`, blast `55*force`. Forces are bounded to 0–8; explicit momentum is bounded to 0–500 N s.

The impulse is applied at the hit point. Lightweight limbs share excess momentum with connected parents. Vertical momentum is retained. The immediate total linear momentum change equals the requested impulse; the old extra angular velocity and random whole-body bullet shove were removed. Joint constraints, contacts and active muscles determine subsequent movement. A bounded directional trunk reflex supplies the visible muscular response, followed by flinching, wound holding, balance or recovery.

Living and dead bodies use the same localized impact path. During dying, leg motors are passive while tissue damping remains; this avoids steering the falling legs back towards standing targets.

### API migration

The last argument of `Character::wound` is now **impulse in N s**, default `-1`, replacing the former `impulse_speed` argument in m/s. The former argument was ignored for living bodies. Callers supplying it must update their units.

`game/src/pedestrians.cpp` now supplies `sqrt(2*m*energy)` using an **8 g reference projectile**, because its hitscan API supplies energy but no projectile mass. This is an explicit approximation. A weapon-specific projectile mass and momentum-transfer fraction should be passed when the game's weapon API supports them. Melee contacts continue through `Character::melee` and the same hit handler.

## Validation

The animation suite covers IK continuity, target reach, support-foot locking, action cancellation, held poses, mirrored knife ownership, local hit torque, momentum conservation, live/dead wound impulses, injury behavior, collapse and getting up. The foundry's native audit runs all 88 actions plus 16 impact cases and three recoveries on each backend.

This remains an assisted physical character controller. The audit detects discontinuities and regressions; it does not establish biomechanical accuracy for every body shape, target or terrain.
