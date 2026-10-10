package dev.hl2craft.world;

import static dev.hl2craft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.hl2craft.HL2Craft;
import dev.hl2craft.link.HL2Link;
import java.lang.foreign.MemorySegment;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.shapes.BitSetDiscreteVoxelShape;
import net.minecraft.world.phys.shapes.CubeVoxelShape;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

public final class HL2Collision {
	public static final int REGION_SIZE = 8;

	private static final ConcurrentHashMap<Long, VoxelShape> SHAPES = new ConcurrentHashMap<>();
	private static volatile int epoch = -1;
	private static Thread consumer;

	private HL2Collision() {
	}

	public static @Nullable VoxelShape shapeAt(BlockPos pos) {
		return SHAPES.isEmpty() ? null : SHAPES.get(pos.asLong());
	}

	public static boolean hasGeometry(BlockPos pos) {
		return SHAPES.containsKey(pos.asLong());
	}

	public static int blockCount() {
		return SHAPES.size();
	}

	public static synchronized void startConsumer() {
		if (consumer != null) {
			return;
		}
		consumer = new Thread(HL2Collision::consumeLoop, "HL2Craft collision");
		consumer.setDaemon(true);
		consumer.start();
	}

	private static void consumeLoop() {
		while (true) {
			try {
				if (!drainOnce()) {
					Thread.sleep(2);
				}
			} catch (InterruptedException e) {
				return;
			} catch (Throwable t) {
				HL2Craft.LOG.error("HL2Craft: collision consumer error", t);
				try {
					Thread.sleep(200);
				} catch (InterruptedException e) {
					return;
				}
			}
		}
	}

	private static boolean drainOnce() {
		MemorySegment s = HL2Link.segment();
		if (s == null) {
			return false;
		}
		long head = HL2Link.collisionHead();
		long tail = HL2Link.collisionTail();
		if (tail >= head) {
			return false;
		}
		long data = OFF_COLLISION_RING + CR_DATA;
		while (tail < head) {
			long pos = tail % CR_DATA_BYTES;
			int type = s.get(JAVA_INT, data + pos);
			int payloadBytes = s.get(JAVA_INT, data + pos + 4);
			if (type == COL_PAD) {
				tail += CR_DATA_BYTES - pos;
				continue;
			}
			long payload = data + pos + 8;
			switch (type) {
				case COL_CLEAR -> clear(s.get(JAVA_INT, payload));
				case COL_REGION -> readRegion(s, payload);
				default -> {}
			}
			tail += (8 + payloadBytes + 7) & ~7L;
		}
		HL2Link.setCollisionTail(tail);
		return true;
	}

	private static final java.util.Set<Long> BARRIER_BLOCKS = java.util.Collections.newSetFromMap(new ConcurrentHashMap<>());
	private static final java.util.Set<BlockPos> PLACED_BLOCKS = java.util.Collections.newSetFromMap(new ConcurrentHashMap<>());
	private static volatile boolean pendingClear = false;

	public static void trackPlacedBlock(BlockPos pos) {
		PLACED_BLOCKS.add(pos.immutable());
	}

	public static boolean untrackPlacedBlock(BlockPos pos) {
		return PLACED_BLOCKS.remove(pos);
	}

	public static boolean isPlacedBlock(BlockPos pos) {
		return PLACED_BLOCKS.contains(pos);
	}

	private static void clear(int newEpoch) {
		SHAPES.clear();
		epoch = newEpoch;
		pendingClear = true;
		HL2Craft.LOG.info("HL2Craft: collision cleared (epoch {})", newEpoch);
	}

	public static void clearAllEntities(net.minecraft.server.level.ServerLevel level) {
		if (level == null) return;
		try {
			java.util.List<net.minecraft.world.entity.Entity> toDiscard = new java.util.ArrayList<>();
			for (net.minecraft.world.entity.Entity e : level.getAllEntities()) {
				if (e != null && !(e instanceof net.minecraft.world.entity.player.Player)) {
					toDiscard.add(e);
				}
			}
			for (net.minecraft.world.entity.Entity e : toDiscard) {
				e.discard();
			}
		} catch (Exception e) {
			HL2Craft.LOG.warn("HL2Craft: Error clearing entities: {}", e.getMessage());
		}
	}

	public static void clearAllPlacedBlocks(net.minecraft.server.level.ServerLevel level) {
		if (!PLACED_BLOCKS.isEmpty()) {
			for (BlockPos bp : PLACED_BLOCKS) {
				if (level != null && bp != null) {
					if (!level.isLoaded(bp)) {
						level.getChunk(bp.getX() >> 4, bp.getZ() >> 4);
					}
					if (!level.getBlockState(bp).isAir()) {
						level.setBlock(bp, net.minecraft.world.level.block.Blocks.AIR.defaultBlockState(), 3);
					}
				}
			}
			PLACED_BLOCKS.clear();
		}

		if (level != null) {
			try {
				var players = level.players();
				if (!players.isEmpty()) {
					var p = players.getFirst();
					int pcx = p.chunkPosition().x();
					int pcz = p.chunkPosition().z();
					for (int cx = pcx - 10; cx <= pcx + 10; cx++) {
						for (int cz = pcz - 10; cz <= pcz + 10; cz++) {
							var chunk = level.getChunkSource().getChunkNow(cx, cz);
							if (chunk != null) {
								var sections = chunk.getSections();
								for (int secIdx = 0; secIdx < sections.length; secIdx++) {
									var sec = sections[secIdx];
									if (sec != null && !sec.hasOnlyAir()) {
										int secBottomY = chunk.getSectionYFromSectionIndex(secIdx) << 4;
										for (int bx = 0; bx < 16; bx++) {
											for (int bz = 0; bz < 16; bz++) {
												for (int by = 0; by < 16; by++) {
													var st = sec.getBlockState(bx, by, bz);
													if (!st.isAir()) {
														BlockPos wp = new BlockPos((cx << 4) + bx, secBottomY + by, (cz << 4) + bz);
														level.setBlock(wp, net.minecraft.world.level.block.Blocks.AIR.defaultBlockState(), 3);
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			} catch (Exception ex) {
				HL2Craft.LOG.warn("HL2Craft: Error scanning chunk sections: {}", ex.getMessage());
			}
		}

		HL2Link.pushEvent(dev.hl2craft.link.Proto.EV_CLEAR_BLOCKS, 0, 0, 0, 0, 0, 0, 0);
		clearAllEntities(level);
	}

	public static void serverTick(net.minecraft.server.MinecraftServer server) {
		if (server == null || !HL2Link.active()) {
			return;
		}
		var players = server.getPlayerList().getPlayers();
		if (players.isEmpty()) {
			return;
		}
		var player = players.getFirst();
		net.minecraft.server.level.ServerLevel level = player.level();
		if (level == null) {
			return;
		}

		boolean playerDead = player.isDeadOrDying();
		if (pendingClear || playerDead) {
			clearAllPlacedBlocks(level);
		}

		if (pendingClear || !BARRIER_BLOCKS.isEmpty()) {
			pendingClear = false;
			for (long packed : BARRIER_BLOCKS) {
				BlockPos bp = BlockPos.of(packed);
				if (level.isLoaded(bp) && level.getBlockState(bp).is(net.minecraft.world.level.block.Blocks.BARRIER)) {
					level.setBlock(bp, net.minecraft.world.level.block.Blocks.AIR.defaultBlockState(), 2);
				}
			}
			BARRIER_BLOCKS.clear();
		}

		// Sweep any legacy barrier blocks saved on disk in chunks near the player
		if ((server.getTickCount() & 19) == 0) {
			int pcx = player.chunkPosition().x();
			int pcz = player.chunkPosition().z();
			for (int cx = pcx - 2; cx <= pcx + 2; cx++) {
				for (int cz = pcz - 2; cz <= pcz + 2; cz++) {
					var chunk = level.getChunkSource().getChunkNow(cx, cz);
					if (chunk != null) {
						var sections = chunk.getSections();
						for (int secIdx = 0; secIdx < sections.length; secIdx++) {
							var sec = sections[secIdx];
							if (sec != null && !sec.hasOnlyAir()) {
								int secBottomY = chunk.getSectionYFromSectionIndex(secIdx) << 4;
								for (int bx = 0; bx < 16; bx++) {
									for (int bz = 0; bz < 16; bz++) {
										for (int by = 0; by < 16; by++) {
											if (sec.getBlockState(bx, by, bz).is(net.minecraft.world.level.block.Blocks.BARRIER)) {
												BlockPos wp = new BlockPos((cx << 4) + bx, secBottomY + by, (cz << 4) + bz);
												level.setBlock(wp, net.minecraft.world.level.block.Blocks.AIR.defaultBlockState(), 2);
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}

	private static void readRegion(MemorySegment s, long p) {
		int minX = s.get(JAVA_INT, p);
		int minY = s.get(JAVA_INT, p + 4);
		int minZ = s.get(JAVA_INT, p + 8);
		int maxX = s.get(JAVA_INT, p + 12);
		int maxY = s.get(JAVA_INT, p + 16);
		int maxZ = s.get(JAVA_INT, p + 20);
		int msgEpoch = s.get(JAVA_INT, p + 24);
		int count = s.get(JAVA_INT, p + 28);

		if (epoch == -1) {
			epoch = msgEpoch;
		} else if (msgEpoch != epoch) {
			return; // Stale data from previous level
		}

		java.util.HashMap<Long, VoxelShape> next = new java.util.HashMap<>(count * 2);
		long e = p + COL_REGION_HEADER_BYTES;
		for (int i = 0; i < count; i++, e += COL_BLOCK_BYTES) {
			int x = s.get(JAVA_INT, e);
			int y = s.get(JAVA_INT, e + 4);
			int z = s.get(JAVA_INT, e + 8);
			VoxelShape shape = buildShape(s, e + 16);
			if (shape != null) {
				next.put(BlockPos.asLong(x, y, z), shape);
			}
		}
		SHAPES.putAll(next);
		SHAPES.keySet().retainAll(next.keySet());
	}

	private static @Nullable VoxelShape buildShape(MemorySegment s, long bitsOff) {
		boolean any = false;
		boolean full = true;
		long[] layers = new long[8];
		for (int y = 0; y < 8; y++) {
			layers[y] = s.get(JAVA_LONG, bitsOff + y * 8L);
			any |= layers[y] != 0;
			full &= layers[y] == -1L;
		}
		if (!any) {
			return null;
		}
		if (full) {
			return Shapes.block();
		}
		BitSetDiscreteVoxelShape discrete = new BitSetDiscreteVoxelShape(8, 8, 8);
		for (int y = 0; y < 8; y++) {
			long layer = layers[y];
			while (layer != 0) {
				int bit = Long.numberOfTrailingZeros(layer);
				layer &= layer - 1;
				discrete.fill(bit & 7, y, bit >>> 3);
			}
		}
		return new CubeVoxelShape(discrete);
	}
}
