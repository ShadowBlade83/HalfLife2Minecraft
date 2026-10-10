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
		return EntityDimensions.scalable(this.entityData.get(WIDTH), this.entityData.get(HEIGHT));
	}

	@Override
	public HumanoidArm getMainArm() {
		return HumanoidArm.RIGHT;
	}

	@Override
	public void tick() {
		this.baseTick();
		this.setHealth(this.getMaxHealth());
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
		this.pendingDamage += dmg;
		if (source.getDirectEntity() instanceof Projectile) {
			this.pendingFlags |= 2;
		}
		this.pendingWeapon = weaponClass(source);
		if (source.is(net.minecraft.tags.DamageTypeTags.IS_FIRE)) {
			this.pendingFlags |= 8;
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
