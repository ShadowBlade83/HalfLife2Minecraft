package dev.hl2craft.combat;

import dev.hl2craft.link.Proto;
import net.minecraft.network.syncher.EntityDataAccessor;
import net.minecraft.network.syncher.EntityDataSerializers;
import net.minecraft.network.syncher.SynchedEntityData;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.tags.ItemTags;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityDimensions;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.HumanoidArm;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Pose;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.Level;

public class HL2ActorEntity extends LivingEntity {
	private static final EntityDataAccessor<Integer> ENT_INDEX = SynchedEntityData.defineId(HL2ActorEntity.class, EntityDataSerializers.INT);
	private static final EntityDataAccessor<Float> WIDTH = SynchedEntityData.defineId(HL2ActorEntity.class, EntityDataSerializers.FLOAT);
	private static final EntityDataAccessor<Float> HEIGHT = SynchedEntityData.defineId(HL2ActorEntity.class, EntityDataSerializers.FLOAT);

	private float pendingDamage;
	private int pendingFlags;
	private int pendingWeapon;
	private double pushX, pushZ;
	private float pushStrength;
	private boolean hitThisTick;

	public HL2ActorEntity(EntityType<? extends HL2ActorEntity> type, Level level) {
		super(type, level);
		this.setNoGravity(true);
		this.noPhysics = true;
		this.setSilent(true);
	}

	@Override
	public double getVisibilityPercent(net.minecraft.server.level.ServerLevel level, net.minecraft.world.entity.Entity lookingEntity) {
		return 1.0;
	}

	@Override
	public boolean canBeSeenAsEnemy() {
		return this.isAlive();
	}

	@Override
	public boolean canBeSeenByAnyone() {
		return true;
	}

	@Override
	public boolean isAttackable() {
		return true;
	}

	@Override
	public boolean isInvisible() {
		return false;
	}

	@Override
	public boolean isInvisibleTo(net.minecraft.world.entity.player.Player player) {
		return false;
	}

	public int entIndex() {
		return this.entityData.get(ENT_INDEX);
	}

	public void setEntIndex(int entIndex) {
		this.entityData.set(ENT_INDEX, entIndex);
	}

	@Override
	protected void defineSynchedData(SynchedEntityData.Builder builder) {
		super.defineSynchedData(builder);
		builder.define(ENT_INDEX, 0);
		builder.define(WIDTH, 0.6F);
		builder.define(HEIGHT, 1.8F);
	}

	public void setSize(float width, float height) {
		if (Math.abs(this.entityData.get(WIDTH) - width) > 0.01F || Math.abs(this.entityData.get(HEIGHT) - height) > 0.01F) {
			this.entityData.set(WIDTH, width);
			this.entityData.set(HEIGHT, height);
			this.refreshDimensions();
		}
	}

	@Override
	public void onSyncedDataUpdated(EntityDataAccessor<?> accessor) {
		super.onSyncedDataUpdated(accessor);
		if (WIDTH.equals(accessor) || HEIGHT.equals(accessor)) {
			this.refreshDimensions();
		}
	}

	@Override
	protected EntityDimensions getDefaultDimensions(Pose pose) {
		float w = this.entityData.get(WIDTH);
		float h = this.entityData.get(HEIGHT);
		if (w <= 0.01F) w = 0.6F;
		if (h <= 0.01F) h = 1.8F;
		float eye = (h >= 1.5F) ? 1.62F : Math.max(0.2F, h * 0.85F);
		return EntityDimensions.scalable(w, h).withEyeHeight(eye);
	}

	@Override
	public HumanoidArm getMainArm() {
		return HumanoidArm.RIGHT;
	}

	@Override
	public void tick() {
		this.baseTick();
		this.setHealth(this.getMaxHealth());
		if (!this.level().isClientSide()) {
			net.minecraft.core.BlockPos pos = this.blockPosition();
			net.minecraft.world.level.material.FluidState fluid = this.level().getFluidState(pos);
			net.minecraft.world.level.material.FluidState fluidAbove = this.level().getFluidState(pos.above());
			net.minecraft.world.level.block.state.BlockState bs = this.level().getBlockState(pos);
			if (fluid.is(net.minecraft.tags.FluidTags.LAVA) || fluidAbove.is(net.minecraft.tags.FluidTags.LAVA)
					|| bs.is(net.minecraft.world.level.block.Blocks.FIRE) || bs.is(net.minecraft.world.level.block.Blocks.SOUL_FIRE)) {
				this.pendingDamage += 4.0F;
				this.pendingFlags |= Proto.HIT_ON_FIRE;
				this.hitThisTick = true;
				this.setRemainingFireTicks(160);
			}
		}
	}

	@Override
	public boolean isPushable() {
		return false;
	}

	@Override
	protected void doPush(Entity entity) {
	}

	@Override
	public boolean canBeCollidedWith(Entity other) {
		return false;
	}

	@Override
	public boolean canBeHitByProjectile() {
		return this.isAlive();
	}

	@Override
	public boolean isPickable() {
		return true;
	}

	@Override
	public boolean shouldShowName() {
		return false;
	}

	@Override
	public boolean hasCustomName() {
		return false;
	}

	@Override
	public net.minecraft.network.chat.Component getCustomName() {
		return null;
	}

	@Override
	public net.minecraft.network.chat.Component getDisplayName() {
		return net.minecraft.network.chat.Component.empty();
	}

	@Override
	public net.minecraft.network.chat.Component getName() {
		return net.minecraft.network.chat.Component.empty();
	}

	@Override
	public boolean shouldBeSaved() {
		return false;
	}

	@Override
	public boolean hurtServer(ServerLevel level, DamageSource source, float dmg) {
		this.actuallyHurt(level, source, dmg);
		return true;
	}

	@Override
	public void die(DamageSource source) {
		this.remove(RemovalReason.DISCARDED);
	}

	@Override
	protected void actuallyHurt(ServerLevel level, DamageSource source, float dmg) {
		if (this.isInvulnerableTo(level, source) || dmg <= 0.0F) {
			return;
		}
		if (source.getEntity() instanceof net.minecraft.server.level.ServerPlayer player && !(source.getDirectEntity() instanceof Projectile)) {
			boolean isFallingCrit = !player.onGround() && !player.onClimbable() && !player.isInWater()
				&& !player.getAbilities().flying && !player.isFallFlying()
				&& (player.fallDistance > 0.0F || player.getDeltaMovement().y < -0.01);
			if (isFallingCrit) {
				this.pendingFlags |= Proto.HIT_CRIT;
				if (player.isSprinting()) {
					dmg *= 1.5F;
				}
				player.crit(this);
				level.sendParticles(
					net.minecraft.core.particles.ParticleTypes.CRIT,
					this.getX(), this.getY() + this.getBbHeight() * 0.65, this.getZ(),
					16, 0.25, 0.35, 0.25, 0.18
				);
				level.playSound(
					null, player.getX(), player.getY(), player.getZ(),
					net.minecraft.sounds.SoundEvents.PLAYER_ATTACK_CRIT,
					net.minecraft.sounds.SoundSource.PLAYERS, 1.0F, 1.0F
				);
			}
		}
		this.pendingDamage += dmg;
		if (source.getDirectEntity() instanceof Projectile) {
			this.pendingFlags |= Proto.HIT_PROJECTILE;
		}
		this.pendingWeapon = weaponClass(source);
		if (source.is(net.minecraft.tags.DamageTypeTags.IS_FIRE)) {
			this.pendingFlags |= Proto.HIT_ON_FIRE;
		}
		if (this.pushStrength <= 0.01F && source.getEntity() instanceof LivingEntity attacker) {
			double dx = this.getX() - attacker.getX();
			double dz = this.getZ() - attacker.getZ();
			double len = Math.hypot(dx, dz);
			if (len > 1e-4) {
				this.pushX = dx / len;
				this.pushZ = dz / len;
			} else {
				var look = attacker.getLookAngle();
				double lLen = Math.hypot(look.x, look.z);
				if (lLen > 1e-4) {
					this.pushX = look.x / lLen;
					this.pushZ = look.z / lLen;
				}
			}
			this.pushStrength = attacker.isSprinting() ? 0.85F : 0.5F;
		}
		this.hitThisTick = true;
		this.setHealth(20.0F);
	}

	@Override
	public void knockback(double power, double xd, double zd, DamageSource source, float damage, boolean comesFromEffect) {
		double len = Math.sqrt(xd * xd + zd * zd);
		if (len > 1e-6 && power > this.pushStrength) {
			this.pushStrength = (float) power;
			this.pushX = -xd / len;
			this.pushZ = -zd / len;
		}
		this.hitThisTick = true;
	}

	private static int weaponClass(DamageSource source) {
		if (source.getDirectEntity() instanceof Projectile) {
			return Proto.WEAPON_ARROW;
		}
		ItemStack weapon = source.getWeaponItem();
		if (weapon == null && source.getEntity() instanceof LivingEntity attacker) {
			weapon = attacker.getMainHandItem();
		}
		if (weapon == null || weapon.isEmpty()) {
			return Proto.WEAPON_UNARMED;
		}
		if (weapon.is(ItemTags.SWORDS)) {
			return Proto.WEAPON_BLADE;
		}
		if (weapon.is(ItemTags.AXES)) {
			return Proto.WEAPON_AXE;
		}
		return Proto.WEAPON_PICKAXE;
	}

	public float[] takeHit() {
		if (!this.hitThisTick) {
			return null;
		}
		float[] hit = {
			this.pendingDamage,
			(float) this.pushX,
			(float) this.pushZ,
			this.pushStrength,
			Float.intBitsToFloat(this.pendingFlags),
			Float.intBitsToFloat(this.pendingWeapon)
		};
		this.pendingDamage = 0.0F;
		this.pendingFlags = 0;
		this.pushX = this.pushZ = 0.0;
		this.pushStrength = 0.0F;
		this.hitThisTick = false;
		return hit;
	}
}
