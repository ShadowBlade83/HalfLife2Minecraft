package dev.hl2craft.mixin;

import dev.hl2craft.combat.HL2ActorEntity;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.goal.target.NearestAttackableTargetGoal;
import net.minecraft.world.entity.monster.Enemy;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Mob.class)
public abstract class MobMixin {
	@org.spongepowered.asm.mixin.Shadow
	protected net.minecraft.world.entity.ai.goal.GoalSelector targetSelector;

	@Inject(method = "<init>", at = @At("RETURN"))
	private void hl2craft$targetHL2Actors(EntityType<? extends Mob> type, Level level, CallbackInfo ci) {
		if (((Object) this) instanceof Enemy) {
			Mob self = (Mob) (Object) this;
			if (this.targetSelector != null) {
				this.targetSelector.addGoal(2, new NearestAttackableTargetGoal<>(self, HL2ActorEntity.class, true));
			}
		}
	}
}
