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
	@Inject(method = "tick", at = @At("HEAD"), cancellable = true)
	private void hl2craft$followHost(final CallbackInfo ci) {
		LocalPlayer self = (LocalPlayer) (Object) this;
		if (self.isFallFlying()) {
			self.setOnGround(false);
			return;
		}
		HL2Client.Pose p = HL2Client.currentPose();
		if (p == null) return;
		double eyeHeight = self.getEyeHeight();
		double feetY = p.y() - eyeHeight;
		self.setPos(p.x(), feetY, p.z());
		self.xo = self.xOld = p.x();
		self.yo = self.yOld = feetY;
		self.zo = self.zOld = p.z();
		self.setYRot(p.yaw());
		self.setXRot(p.pitch());
		self.yRotO = p.yaw();
		self.xRotO = p.pitch();
		self.yHeadRot = p.yaw();
		self.yHeadRotO = p.yaw();
		self.yBodyRot = p.yaw();
		self.yBodyRotO = p.yaw();
		self.setDeltaMovement(Vec3.ZERO);
		dev.hl2craft.link.HL2Link.HostSnapshot snap = dev.hl2craft.link.HL2Link.readHost();
		if (snap != null) {
			self.setOnGround(snap.onGround());
		}
	}
}
