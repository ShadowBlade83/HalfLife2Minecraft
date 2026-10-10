package dev.hl2craft.mixin;

import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import net.minecraft.core.BlockPos;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.context.BlockPlaceContext;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(BlockItem.class)
public abstract class BlockItemMixin {
	@Inject(method = "place", at = @At("HEAD"), cancellable = true)
	private void hl2craft$preventPlaceInsideHl2Player(BlockPlaceContext context, CallbackInfoReturnable<InteractionResult> cir) {
		if (!HL2Link.active() || context == null) {
			return;
		}
		BlockPos pos = context.getClickedPos();
		BlockItem self = (BlockItem) (Object) this;
		BlockState defaultState = self.getBlock().defaultBlockState();
		if (defaultState.getCollisionShape(context.getLevel(), pos).isEmpty()) {
			return;
		}

		// HL2 player hull is 32x32x72 Source units (+/- 0.4064m wide, 1.8288m tall),
		// which is wider than Minecraft's 0.6m (+/- 0.30m) player hitbox.
		final double halfW = 0.415;
		final double bx0 = pos.getX();
		final double bx1 = bx0 + 1.0;
		final double by0 = pos.getY();
		final double by1 = by0 + 1.0;
		final double bz0 = pos.getZ();
		final double bz1 = bz0 + 1.0;

		Player player = context.getPlayer();
		boolean crouching = player != null && player.isShiftKeyDown();
		double pHeight = crouching ? 0.92 : 1.82;

		if (player != null) {
			double px = player.getX();
			double py = player.getY();
			double pz = player.getZ();
			if (px + halfW > bx0 && px - halfW < bx1 &&
				pz + halfW > bz0 && pz - halfW < bz1 &&
				py + pHeight > by0 + 0.02 && py + 0.02 < by1) {
				cir.setReturnValue(InteractionResult.FAIL);
				return;
			}
		}

		HL2Link.HostSnapshot snap = HL2Link.readHost();
		if (snap != null && snap.inPlayable()) {
			double hx = snap.x();
			double hz = snap.z();
			double eyeH = crouching ? (28.0 * 0.0254) : (64.0 * 0.0254);
			double hy = snap.y() - eyeH;
			if (hx + halfW > bx0 && hx - halfW < bx1 &&
				hz + halfW > bz0 && hz - halfW < bz1 &&
				hy + pHeight > by0 + 0.02 && hy + 0.02 < by1) {
				cir.setReturnValue(InteractionResult.FAIL);
			}
		}
	}

	@Inject(method = "place", at = @At("RETURN"))
	private void hl2craft$onBlockPlaced(BlockPlaceContext context, CallbackInfoReturnable<InteractionResult> cir) {
		if (HL2Link.active() && cir.getReturnValue().consumesAction()) {
			BlockPos pos = context.getClickedPos();
			BlockState placed = context.getLevel().getBlockState(pos);
			if (!placed.getCollisionShape(context.getLevel(), pos).isEmpty()) {
				dev.hl2craft.world.HL2Collision.trackPlacedBlock(pos);
				HL2Link.pushEvent(Proto.EV_BLOCK_PLACED, 0, pos.getX(), pos.getY(), pos.getZ(), 0.0f, 0, 0);
			}
		}
	}
}
