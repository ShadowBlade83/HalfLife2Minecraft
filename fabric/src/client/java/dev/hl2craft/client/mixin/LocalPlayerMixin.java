package dev.hl2craft.client.mixin;

import dev.hl2craft.client.HL2Client;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.player.Abilities;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(LocalPlayer.class)
abstract class LocalPlayerMixin {
	private static int lastClientTeleportSeq = -1;
	private static int lastClientEpoch = -1;

	@Inject(method = "tick", at = @At("HEAD"), cancellable = true)
	private void hl2craft$followHost(final CallbackInfo ci) {
		LocalPlayer self = (LocalPlayer) (Object) this;
		dev.hl2craft.link.HL2Link.HostSnapshot snap = dev.hl2craft.link.HL2Link.readHost();
		if (snap == null || !snap.inPlayable()) {
			return;
		}

		boolean teleportChanged = (lastClientTeleportSeq == -1 || snap.teleportSeq() != lastClientTeleportSeq || snap.epoch() != lastClientEpoch);
		lastClientTeleportSeq = snap.teleportSeq();
		lastClientEpoch = snap.epoch();

		double hostFeetY = snap.y() - self.getEyeHeight();
		dev.hl2craft.world.HL2Collision.ensurePlayerFloorAndStep(snap.x(), hostFeetY, snap.z(), hostFeetY);

		if (teleportChanged || snap.hl2PhysicsOverride()) {
			self.setPos(snap.x(), hostFeetY, snap.z());
			self.xo = self.xOld = snap.x();
			self.yo = self.yOld = hostFeetY;
			self.zo = self.zOld = snap.z();
			self.setYRot(snap.yaw());
			self.setXRot(snap.pitch());
			self.yRotO = snap.yaw();
			self.xRotO = snap.pitch();
			self.yHeadRot = snap.yaw();
			self.yHeadRotO = snap.yaw();
			self.yBodyRot = snap.yaw();
			self.yBodyRotO = snap.yaw();
			self.setDeltaMovement(Vec3.ZERO);
			self.setOnGround(snap.onGround());
			return;
		}

		dev.hl2craft.world.HL2Collision.ensurePlayerFloorAndStep(self.getX(), self.getY(), self.getZ(), hostFeetY);
		if (self.isFallFlying()) {
			self.setOnGround(false);
		}
	}

	@Inject(method = "tick", at = @At("TAIL"))
	private void hl2craft$afterTickSlopesAndLadders(final CallbackInfo ci) {
		LocalPlayer self = (LocalPlayer) (Object) this;
		dev.hl2craft.link.HL2Link.HostSnapshot snap = dev.hl2craft.link.HL2Link.readHost();
		if (snap == null || !snap.inPlayable() || snap.hl2PhysicsOverride()) {
			return;
		}

		if (snap.onLadder() && !self.getAbilities().flying && !self.isFallFlying()) {
			self.fallDistance = 0.0f;
			Vec3 dm = self.getDeltaMovement();
			double vx = Math.clamp(dm.x, -0.15, 0.15);
			double vz = Math.clamp(dm.z, -0.15, 0.15);
			double vy = Math.max(dm.y, -0.15);
			if (self.input != null) {
				if (self.input.keyPresses.forward() || self.input.keyPresses.jump()) {
					vy = 0.22;
				} else if (self.input.keyPresses.shift()) {
					vy = 0.0;
				}
			}
			self.setDeltaMovement(vx, vy, vz);
			return;
		}

		if (!self.getAbilities().flying && !self.isFallFlying() && self.onGround() && self.getDeltaMovement().y <= 0.01) {
			double hostFeetY = snap.y() - self.getEyeHeight();
			double floorTopY = dev.hl2craft.world.HL2Collision.ensurePlayerFloorAndStep(self.getX(), self.getY(), self.getZ(), hostFeetY);
			if (!Double.isNaN(floorTopY) && floorTopY > self.getY() + 0.005 && floorTopY <= self.getY() + 0.68) {
				if (self.level().noCollision(self, self.getBoundingBox().move(0.0, floorTopY - self.getY() + 0.02, 0.0))) {
					self.setPos(self.getX(), floorTopY, self.getZ());
					self.setOnGround(true);
					Vec3 dm = self.getDeltaMovement();
					if (dm.y < 0.0) {
						self.setDeltaMovement(dm.x, 0.0, dm.z);
					}
				}
			} else if (self.horizontalCollision && self.input != null) {
				Vec3 moveVec = self.getLookAngle();
				double len = Math.hypot(moveVec.x, moveVec.z);
				if (len > 1.0e-4 && (self.input.keyPresses.forward() || self.input.keyPresses.backward() || self.input.keyPresses.left() || self.input.keyPresses.right())) {
					double dirX = moveVec.x / len;
					double dirZ = moveVec.z / len;
					double aheadX = self.getX() + dirX * 0.55;
					double aheadZ = self.getZ() + dirZ * 0.55;
					double aheadFloorY = dev.hl2craft.world.HL2Collision.ensurePlayerFloorAndStep(aheadX, self.getY(), aheadZ, hostFeetY);
					if (!Double.isNaN(aheadFloorY) && aheadFloorY > self.getY() + 0.05 && aheadFloorY <= self.getY() + 0.68) {
						double liftY = aheadFloorY - self.getY() + 0.03;
						if (self.level().noCollision(self, self.getBoundingBox().move(dirX * 0.15, liftY, dirZ * 0.15))) {
							self.setPos(self.getX() + dirX * 0.15, aheadFloorY + 0.01, self.getZ() + dirZ * 0.15);
							self.setOnGround(true);
						}
					}
				}
			}
		}
	}
}
