#include "hl2_bridge.h"
#include "d3d9_render.h"
#include <cstring>
#include <setjmp.h>

namespace
{
	static jmp_buf g_traceJmp;
	static volatile bool g_inTrace = false;

	static LONG WINAPI TraceVehHandler(PEXCEPTION_POINTERS)
	{
		if (g_inTrace) {
			g_inTrace = false;
			longjmp(g_traceJmp, 1);
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

namespace hl2craft
{
	HL2Bridge& HL2Bridge::Get()
	{
		static HL2Bridge instance;
		return instance;
	}

	HL2Bridge::HL2Bridge() {}

	HL2Bridge::~HL2Bridge()
	{
		Shutdown();
	}

	static void PatchJumpHeightModule(HMODULE hMod, float targetHeight)
	{
		if (!hMod) return;
		auto *dos = (IMAGE_DOS_HEADER*)hMod;
		if (IsBadReadPtr(dos, sizeof(IMAGE_DOS_HEADER)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
		auto *nt = (IMAGE_NT_HEADERS32*)((uint8_t*)hMod + dos->e_lfanew);
		if (IsBadReadPtr(nt, sizeof(IMAGE_NT_HEADERS32)) || nt->Signature != IMAGE_NT_SIGNATURE) return;
		uint8_t *base = (uint8_t*)hMod;
		uint32_t size = nt->OptionalHeader.SizeOfImage;
		for (uint32_t i = 0x1000; i + 32 < size; ++i) {
			if ((base[i] == 0x74 || base[i] == 0xEB) && base[i+1] == 0x0A &&
			    base[i+2] == 0xF3 && base[i+3] == 0x0F && base[i+4] == 0x10 && base[i+5] == 0x05 &&
			    base[i+10] == 0xEB && base[i+11] == 0x17 && base[i+12] == 0xE8 &&
			    base[i+17] == 0xDC && base[i+18] == 0xC0 && base[i+19] == 0xD8 && base[i+20] == 0x0D) {
				DWORD oldProt = 0;
				if (VirtualProtect(base + i, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
					base[i] = 0xEB; // Always use sqrt(2 * gravity * jumpHeight)
					VirtualProtect(base + i, 2, oldProt, &oldProt);
				}
				float *pJumpHeight = *(float**)(base + i + 21);
				if (pJumpHeight && !IsBadReadPtr(pJumpHeight, sizeof(float))) {
					DWORD oldDataProt = 0;
					if (VirtualProtect(pJumpHeight, sizeof(float), PAGE_READWRITE, &oldDataProt)) {
						*pJumpHeight = targetHeight;
						VirtualProtect(pJumpHeight, sizeof(float), oldDataProt, &oldDataProt);
					}
				}
			}
		}
	}

	static void PatchMinecraftJumpHeight()
	{
		// 52.0 Source units (~1.252 Minecraft blocks actual peak height after first-tick FinishGravity)
		constexpr float kMcJumpHeightUnits = 52.0f;
		PatchJumpHeightModule(GetModuleHandleA("client.dll"), kMcJumpHeightUnits);
		PatchJumpHeightModule(GetModuleHandleA("server.dll"), kMcJumpHeightUnits);
	}

	void HL2Bridge::Init()
	{
		if (m_initialized) return;

		static bool s_vehInstalled = false;
		if (!s_vehInstalled) {
			AddVectoredExceptionHandler(1, TraceVehHandler);
			s_vehInstalled = true;
		}

		InitSharedMemory();

		HMODULE hEngine = GetModuleHandleA("engine.dll");
		HMODULE hClient = GetModuleHandleA("client.dll");
		if (!hEngine || !hClient) return;

		CreateInterfaceFn engineFactory = (CreateInterfaceFn)GetProcAddress(hEngine, "CreateInterface");
		CreateInterfaceFn clientFactory = (CreateInterfaceFn)GetProcAddress(hClient, "CreateInterface");
		if (!engineFactory || !clientFactory) return;

		m_engineClient = engineFactory("VEngineClient014", nullptr);
		if (!m_engineClient)
			m_engineClient = engineFactory("VEngineClient013", nullptr);

		m_engineTrace = engineFactory("EngineTraceClient003", nullptr);
		m_entityList = clientFactory("VClientEntityList003", nullptr);
		m_engineVGui = engineFactory("VEngineVGui001", nullptr);

		HMODULE hVgui = GetModuleHandleA("vguimatsurface.dll");
		if (!hVgui) hVgui = LoadLibraryA("bin\\vguimatsurface.dll");
		if (hVgui) {
			CreateInterfaceFn vguiFactory = (CreateInterfaceFn)GetProcAddress(hVgui, "CreateInterface");
			if (vguiFactory) {
				m_vguiSurface = vguiFactory("VGUI_Surface030", nullptr);
			}
		}

		HMODULE hVstdlib = GetModuleHandleA("vstdlib.dll");
		if (!hVstdlib) hVstdlib = LoadLibraryA("bin\\vstdlib.dll");
		if (hVstdlib) {
			CreateInterfaceFn vstdlibFactory = (CreateInterfaceFn)GetProcAddress(hVstdlib, "CreateInterface");
			if (vstdlibFactory) {
				m_cvar = vstdlibFactory("VEngineCvar004", nullptr);
				if (!m_cvar) m_cvar = vstdlibFactory("VEngineCvar007", nullptr);
			}
		}
		if (!m_cvar && engineFactory) {
			m_cvar = engineFactory("VEngineCvar004", nullptr);
			if (!m_cvar) m_cvar = engineFactory("VEngineCvar007", nullptr);
		}

		PatchMinecraftJumpHeight();
		ExecCmd("mat_antialias 0; mat_aaquality 0; sv_cheats 1; sv_gravity 600; bind shift +duck; hl2_normspeed 190; default_fov 75;");

		InitSharedMemory();
		m_initialized = (m_engineClient != nullptr && m_entityList != nullptr && m_shmView != nullptr);
		HookWindow();
	}

	void HL2Bridge::Shutdown()
	{
		UnhookWindow();
		if (m_shmView) {
			UnmapViewOfFile(m_shmView);
			m_shmView = nullptr;
		}
		if (m_shmHandle) {
			CloseHandle(m_shmHandle);
			m_shmHandle = nullptr;
		}
		m_initialized = false;
	}

	void HL2Bridge::InitSharedMemory()
	{
		if (m_shmView) return;
		m_shmHandle = CreateFileMappingW(
			INVALID_HANDLE_VALUE,
			nullptr,
			PAGE_READWRITE,
			(DWORD)(proto::kMappingBytes >> 32),
			(DWORD)(proto::kMappingBytes & 0xFFFFFFFF),
			proto::kMappingName
		);
		if (!m_shmHandle) {
			m_shmHandle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, proto::kMappingName);
		}
		if (!m_shmHandle) return;

		m_shmView = (uint8_t*)MapViewOfFile(m_shmHandle, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)proto::kMappingBytes);
		if (!m_shmView) return;

		proto::Header *hdr = (proto::Header*)m_shmView;
		hdr->magic = proto::kMagic;
		hdr->version = proto::kVersion;
		hdr->hostPid = GetCurrentProcessId();
		hdr->hostHeartbeatMs = (uint64_t)GetTickCount();
	}

	bool HL2Bridge::IsInGame()
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef bool (__thiscall *Fn)(void*);
		return ((Fn)vt[26])(m_engineClient);
	}

	bool HL2Bridge::IsConnected()
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef bool (__thiscall *Fn)(void*);
		return ((Fn)vt[27])(m_engineClient);
	}

	bool HL2Bridge::IsPaused()
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef bool (__thiscall *Fn)(void*);
		return ((Fn)vt[85])(m_engineClient);
	}

	bool HL2Bridge::IsLevelMainMenuBackground()
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef bool (__thiscall *Fn)(void*);
		return ((Fn)vt[86])(m_engineClient);
	}

	bool HL2Bridge::IsDrawingLoadingImage()
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef bool (__thiscall *Fn)(void*);
		return ((Fn)vt[28])(m_engineClient);
	}

	bool HL2Bridge::HasPlayer()
	{
		Vector p{0, 0, 0};
		return GetPlayerPos(p);
	}

	bool HL2Bridge::IsInPlayableWorld()
	{
		if (!m_engineClient) return false;
		if (!IsInGame() || !IsConnected()) return false;
		if (IsLevelMainMenuBackground()) return false;
		if (IsDrawingLoadingImage()) return false;

		void **vt = *(void***)m_engineClient;
		typedef int (__thiscall *GetLocalPlayerFn)(void*);
		int localPlayer = ((GetLocalPlayerFn)vt[12])(m_engineClient);
		if (localPlayer <= 0) return false;

		return HasPlayer();
	}

	bool HL2Bridge::ExecCmd(const char *cmd)
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef void (__thiscall *ClientCmdFn)(void*, const char*);
		((ClientCmdFn)vt[7])(m_engineClient, cmd);
		return true;
	}

	bool HL2Bridge::GetAngles(QAngle &angles)
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef void (__thiscall *GetViewAnglesFn)(void*, QAngle&);
		((GetViewAnglesFn)vt[19])(m_engineClient, angles);
		return true;
	}

	void* HL2Bridge::GetLocalPlayerEntity()
	{
		if (!m_entityList || !m_engineClient) return nullptr;
		void **evt = *(void***)m_engineClient;
		typedef int (__thiscall *GetLocalPlayerFn)(void*);
		int localPlayer = ((GetLocalPlayerFn)evt[12])(m_engineClient);
		if (localPlayer <= 0) return nullptr;
		void **lvt = *(void***)m_entityList;
		typedef void* (__thiscall *GetEntityFn)(void*, int);
		return ((GetEntityFn)lvt[3])(m_entityList, localPlayer);
	}

	bool HL2Bridge::GetPlayerPos(Vector &pos)
	{
		if (!m_entityList || !IsInGame()) return false;
		void *player = GetLocalPlayerEntity();
		if (!player || IsBadReadPtr(player, sizeof(void*))) return false;

		void *renderable = (char*)player + 4;
		if (IsBadReadPtr(renderable, sizeof(void*))) return false;
		void **rendVt = *(void***)renderable;
		if (IsBadReadPtr(rendVt, sizeof(void*) * 2)) return false;

		typedef const float* (__thiscall *GetRenderOriginFn)(void*);
		const float *org = ((GetRenderOriginFn)rendVt[1])(renderable);
		if (org && !IsBadReadPtr(org, sizeof(float) * 3)) {
			pos.x = org[0]; pos.y = org[1]; pos.z = org[2];
			return true;
		}
		return false;
	}

	bool HL2Bridge::GetWorldToScreenMatrix(float outMat[16])
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		if (!vt || IsBadReadPtr(vt, sizeof(void*) * 38)) return false;
		void *fnPtr = vt[36];
		if (!fnPtr || IsBadCodePtr((FARPROC)fnPtr)) return false;

		typedef const void* (__thiscall *GetW2SFn)(void*);
		const void *pMat = ((GetW2SFn)fnPtr)(m_engineClient);
		if (!pMat || IsBadReadPtr(pMat, sizeof(float) * 16)) return false;

		std::memcpy(outMat, pMat, sizeof(float) * 16);
		return true;
	}

	void HL2Bridge::SetGuiOpen(bool open)
	{
		if (open != m_guiOpen) {
			m_guiOpen = open;
			if (m_guiOpen) {
				POINT pt;
				if (GetCursorPos(&pt) && m_hValveWnd) {
					ScreenToClient(m_hValveWnd, &pt);
					m_cursorX = std::clamp((int)pt.x, 0, m_viewW - 1);
					m_cursorY = std::clamp((int)pt.y, 0, m_viewH - 1);
				} else {
					m_cursorX = m_viewW / 2;
					m_cursorY = m_viewH / 2;
				}
				PushInput(proto::kInCursor, 0, m_cursorX, m_cursorY);
				ExecCmd("cl_mouseenable 0; -forward; -back; -moveleft; -moveright; -jump; -duck; -speed; -attack; -attack2;");
				ShowCursor(TRUE);
			} else {
				ExecCmd("cl_mouseenable 1;");
				ShowCursor(FALSE);
				PushInput(proto::kInReleaseAll, 0, 0, 0);
			}
		}
	}

	void HL2Bridge::Tick()
	{
		if (!m_initialized) {
			Init();
			if (!m_initialized) return;
		}

		proto::Header *hdr = (proto::Header*)m_shmView;
		if (hdr) {
			hdr->hostHeartbeatMs = (uint64_t)GetTickCount();
		}

		char currentMap[64] = {0};
		if (m_engineClient) {
			void **evt = *(void***)m_engineClient;
			typedef const char* (__thiscall *GetLevelNameFn)(void*);
			const char *lvl = ((GetLevelNameFn)evt[53])(m_engineClient);
			if (lvl) {
				const char *slash = strrchr(lvl, '/');
				if (!slash) slash = strrchr(lvl, '\\');
				const char *name = slash ? slash + 1 : lvl;
				strncpy(currentMap, name, sizeof(currentMap) - 1);
			}
		}

		bool inPlayable = IsInPlayableWorld();
		if (!inPlayable) {
			if (m_wasInWorld) {
				m_wasInWorld = false;
				m_lastPosValid = false;
			}
			return;
		}

		// Level change, quicksave load, or checkpoint reload detection
		bool isLoading = IsDrawingLoadingImage();
		bool levelChanged = (currentMap[0] != '\0' && strcmp(currentMap, m_lastMapName) != 0);
		bool saveReloaded = (m_wasLoading && !isLoading);

		float curGameTime = 0.0f;
		if (m_engineClient) {
			void **vt = *(void***)m_engineClient;
			typedef float (__thiscall *TimeFn)(void*);
			curGameTime = ((TimeFn)vt[14])(m_engineClient);
		}
		bool timeJumpedBack = (m_lastGameTime > 3.0f && curGameTime < m_lastGameTime - 2.0f);
		m_lastGameTime = curGameTime;
		m_wasLoading = isLoading;

		if (levelChanged || saveReloaded || timeJumpedBack) {
			if (levelChanged) {
				strncpy(m_lastMapName, currentMap, sizeof(m_lastMapName) - 1);
			}
			m_collisionEpoch++;
			m_placedBlocks.clear();
			D3D9WorldRender::Get().ClearSections();
			ExecCmd("ent_fire mc_blk_* Kill");
			if (m_shmView) {
				uint64_t head = *(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff);
				uint64_t pos = head % proto::kColRingDataBytes;
				uint8_t *slot = m_shmView + proto::kOffCollisionRing + proto::kColRingDataOff + pos;
				*(uint32_t*)slot = proto::kColClear;
				*(uint32_t*)(slot + 4) = 4;
				*(uint32_t*)(slot + 8) = m_collisionEpoch;
				*(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff) = head + 16;
			}
		}

		Vector pos{0, 0, 0};
		QAngle angles{0, 0, 0};
		if (!GetPlayerPos(pos) || !GetAngles(angles)) {
			return;
		}

		if (!m_wasInWorld) {
			m_wasInWorld = true;
			HookWindow();
			PatchMinecraftJumpHeight();
			ExecCmd("sv_cheats 1; sv_gravity 600; mat_queue_mode 0; unbind ctrl; unbind lctrl; unbind rctrl; bind shift +duck; bind lshift +duck; bind ctrl +speed; bind lctrl +speed; hl2_normspeed 190; r_drawviewmodel 0; ent_fire !player StripWeapons;");
		}
		m_lastPosX = pos.x;
		m_lastPosY = pos.y;
		m_lastPosZ = pos.z;
		m_lastPosValid = true;

		UpdateHostState(pos, angles);
		ProcessInput();
		ProcessEvents();

		DWORD now = GetTickCount();
		if (now - m_lastStripTime > 2500) {
			m_lastStripTime = now;
			ExecCmd("firstperson; r_drawviewmodel 0; mat_queue_mode 0; ent_fire !player addoutput \"effects 0\"; ent_fire !player StripWeapons; mat_vsync 0; fps_max 0; engine_no_focus_sleep 0;");
		}

		float dMovedSq = (pos.x - m_lastScanPos.x)*(pos.x - m_lastScanPos.x) +
		                 (pos.y - m_lastScanPos.y)*(pos.y - m_lastScanPos.y) +
		                 (pos.z - m_lastScanPos.z)*(pos.z - m_lastScanPos.z);
		if (dMovedSq > 40000.0f || (now - m_lastColScanTime > 3000)) {
			m_lastColScanTime = now;
			m_lastScanPos = pos;
			ScanCollision(pos);
		}
		if (now - m_lastActorScanTime > 50) {
			m_lastActorScanTime = now;
			ScanActors();
		}
	}

	static bool IsNpcClass(const char *className);
	static bool IsPropClass(const char *className);

	void HL2Bridge::UpdateHostState(const Vector &pos, const QAngle &angles)
	{
		if (!m_shmView) return;
		uint8_t *base = m_shmView + proto::kOffHostState;

		constexpr double kYOffset = 64.0;
		bool isDucking = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		float eyeHeightUnits = isDucking ? 28.0f : 64.0f;

		// Convert Source coords -> MC coords
		double feet_x = pos.x * proto::kMetersPerUnit;
		double feet_y = (pos.z * proto::kMetersPerUnit) + kYOffset;
		double feet_z = -pos.y * proto::kMetersPerUnit;

		double cam_x = feet_x;
		double cam_y = ((pos.z + eyeHeightUnits) * proto::kMetersPerUnit) + kYOffset;
		double cam_z = feet_z;

		// Convert Source angles -> MC angles
		// Source: Yaw increases CCW (0 = East/+X, 90 = North/+Y).
		// MC: Yaw increases CW (0 = South/+Z, 90 = West/-X).
		float mc_yaw = -90.0f - angles.yaw;
		while (mc_yaw > 180.0f) mc_yaw -= 360.0f;
		while (mc_yaw < -180.0f) mc_yaw += 360.0f;
		float mc_pitch = angles.pitch;

		proto::HostState *hs = (proto::HostState*)(m_shmView + proto::kOffHostState);
		uint32_t seq = hs->seq;
		hs->seq = seq + 1;
		MemoryBarrier();

		uint32_t flags = proto::kHostInGame;
		if (IsPaused()) flags |= proto::kHostPaused;

		Vector down = pos;
		down.z -= 6.0f;
		trace_t tr;
		if (TraceWorldRay(pos, down, tr) && tr.fraction < 1.0f) {
			flags |= proto::kHostOnGround;
		}

		hs->flags = flags;
		hs->mapId = 1;
		hs->collisionEpoch = m_collisionEpoch;
		hs->posX = cam_x;
		hs->posY = cam_y;
		hs->posZ = cam_z;
		hs->yaw = mc_yaw;
		hs->pitch = mc_pitch;

		HWND hwnd = FindWindowA("Valve001", nullptr);
		RECT rc;
		if (hwnd && GetClientRect(hwnd, &rc)) {
			hs->viewportW = rc.right - rc.left;
			hs->viewportH = rc.bottom - rc.top;
		} else {
			hs->viewportW = 1920;
			hs->viewportH = 1080;
		}
		m_viewW = hs->viewportW;
		m_viewH = hs->viewportH;

		float hl2Fov = 75.0f;
		float radFov = hl2Fov * 0.5f * 0.0174532925f;
		float vertFov = 2.0f * std::atan(std::tan(radFov) * 0.75f) * 57.2957795f;
		hs->fov = vertFov;
		hs->gameMode = m_creative ? 1 : 0;

		MemoryBarrier();
		hs->seq = seq + 2;

		// Check McState to see if Minecraft screen is open
		proto::McState *ms = (proto::McState*)(m_shmView + proto::kOffMcState);
		bool mcScreenOpen = (ms->flags & proto::kMcScreenOpen) != 0;
		SetGuiOpen(mcScreenOpen);

		// Synchronize Elytra flight / flying from Minecraft -> HL2
		bool mcFlying = (ms->flags & proto::kMcFlying) != 0;
		if (mcFlying) {
			if (!m_wasFlying) {
				m_wasFlying = true;
				ExecCmd("ent_fire !player addoutput \"gravity 0.05\"");
			}
			float sx = (float)ms->x * (float)proto::kUnitsPerBlock;
			float sy = -(float)ms->z * (float)proto::kUnitsPerBlock;
			float sz = ((float)ms->y - 64.0f) * (float)proto::kUnitsPerBlock;
			char flyCmd[128];
			snprintf(flyCmd, sizeof(flyCmd), "setpos %.2f %.2f %.2f", sx, sy, sz);
			ExecCmd(flyCmd);
		} else if (m_wasFlying) {
			m_wasFlying = false;
			ExecCmd("ent_fire !player addoutput \"gravity 1\"");
		} else if (!m_placedBlocks.empty()) {
			int pbx = (int)std::floor(feet_x);
			int pby = (int)std::floor(feet_y);
			int pbz = (int)std::floor(feet_z);
			float pMinX = pos.x - 14.0f, pMaxX = pos.x + 14.0f;
			float pMinY = pos.y - 14.0f, pMaxY = pos.y + 14.0f;
			float pMinZ = pos.z + 2.0f,  pMaxZ = pos.z + 69.0f;
			bool unstuck = false;
			for (int dy = 0; dy <= 1 && !unstuck; ++dy) {
				for (int dx = -1; dx <= 1 && !unstuck; ++dx) {
					for (int dz = -1; dz <= 1 && !unstuck; ++dz) {
						int ibx = pbx + dx, iby = pby + dy, ibz = pbz + dz;
						if (m_placedBlocks.find({ibx, iby, ibz}) != m_placedBlocks.end()) {
							float bMinX = (float)ibx * (float)proto::kUnitsPerBlock;
							float bMaxX = ((float)ibx + 1.0f) * (float)proto::kUnitsPerBlock;
							float bMinY = -((float)ibz + 1.0f) * (float)proto::kUnitsPerBlock;
							float bMaxY = -(float)ibz * (float)proto::kUnitsPerBlock;
							float bMinZ = ((float)iby - 64.0f) * (float)proto::kUnitsPerBlock;
							float bMaxZ = ((float)iby + 1.0f - 64.0f) * (float)proto::kUnitsPerBlock;
							if (pMinX < bMaxX && pMaxX > bMinX && pMinY < bMaxY && pMaxY > bMinY && pMinZ < bMaxZ && pMaxZ > bMinZ) {
								Vector targetPos = pos;
								ComputeUnstickPos(pos, ibx, iby, ibz, targetPos);
								char unstickCmd[128];
								snprintf(unstickCmd, sizeof(unstickCmd), "setpos %.2f %.2f %.2f", targetPos.x, targetPos.y, targetPos.z);
								ExecCmd(unstickCmd);
								unstuck = true;
							}
						}
					}
				}
			}
		}

		// Synchronize damage taken in HL2 -> Minecraft
		bool mcBlocking = (ms->flags & proto::kMcBlocking) != 0;
		if (m_entityList && m_engineClient && !m_creative) {
			void *pPlayer = GetLocalPlayerEntity();
			if (pPlayer && !IsBadReadPtr((char*)pPlayer + 0x90, 4)) {
				int curHp = *(int*)((char*)pPlayer + 0x90);
				if (m_lastPlayerHl2Hp > 0 && curHp < m_lastPlayerHl2Hp) {
					int dmg = m_lastPlayerHl2Hp - curHp;
					bool blocked = false;
					if (mcBlocking) {
						constexpr float kDeg2Rad = 0.0174532925f;
						float radYaw = angles.yaw * kDeg2Rad;
						float radPitch = angles.pitch * kDeg2Rad;
						Vector fwd{
							std::cos(radPitch) * std::cos(radYaw),
							std::cos(radPitch) * std::sin(radYaw),
							-std::sin(radPitch)
						};

						// Find closest hostile/active NPC within 2500 units
						float bestDistSq = 2500.0f * 2500.0f;
						Vector bestAttackerPos{0, 0, 0};
						bool foundAttacker = false;

						void **lvt = *(void***)m_entityList;
						typedef void* (__thiscall *GetEntityFn)(void*, int);
						typedef int (__thiscall *GetHighestFn)(void*);
						GetEntityFn getEntity = (GetEntityFn)lvt[3];
						GetHighestFn getHighest = (GetHighestFn)lvt[6];
						int maxEnt = getHighest ? getHighest(m_entityList) : 256;
						maxEnt = std::clamp(maxEnt, 1, 512);

						for (int i = 2; i <= maxEnt; ++i) {
							void *ent = getEntity(m_entityList, i);
							if (!ent || ent == pPlayer) continue;

							void *networkable = (char*)ent + 8;
							if (IsBadReadPtr(networkable, sizeof(void*) * 9)) continue;
							void **netVt = *(void***)networkable;
							if (IsBadReadPtr(netVt, sizeof(void*) * 9)) continue;

							typedef void* (__thiscall *GetClientClassFn)(void*);
							typedef bool (__thiscall *IsDormantFn)(void*);
							void *clientClass = ((GetClientClassFn)netVt[2])(networkable);
							if (!clientClass || IsBadReadPtr(clientClass, sizeof(void*) * 3)) continue;
							const char *className = *(const char**)((char*)clientClass + 8);
							if (!className || !IsNpcClass(className)) continue;
							if (((IsDormantFn)netVt[8])(networkable)) continue;

							void *renderable = (char*)ent + 4;
							if (IsBadReadPtr(renderable, sizeof(void*))) continue;
							void **rendVt = *(void***)renderable;
							if (IsBadReadPtr(rendVt, sizeof(void*) * 2)) continue;
							typedef const float* (__thiscall *GetOriginFn)(void*);
							const float *org = ((GetOriginFn)rendVt[1])(renderable);
							if (!org || IsBadReadPtr(org, sizeof(float) * 3)) continue;

							float dx = org[0] - pos.x;
							float dy = org[1] - pos.y;
							float dz = org[2] - pos.z;
							float dSq = dx * dx + dy * dy + dz * dz;
							if (dSq < bestDistSq) {
								bestDistSq = dSq;
								bestAttackerPos = { org[0], org[1], org[2] };
								foundAttacker = true;
							}
						}

						if (foundAttacker) {
							float dirX = bestAttackerPos.x - pos.x;
							float dirY = bestAttackerPos.y - pos.y;
							float dirLen = std::sqrt(dirX * dirX + dirY * dirY);
							if (dirLen > 1.0f) {
								dirX /= dirLen;
								dirY /= dirLen;
								float dot = fwd.x * dirX + fwd.y * dirY;
								// Shield in MC blocks in 180° frontal arc
								if (dot > -0.1f) {
									blocked = true;
								}
							} else {
								blocked = true;
							}
						}
					}

					if (blocked) {
						// Shield absorbs the damage!
						*(int*)((char*)pPlayer + 0x90) = m_lastPlayerHl2Hp;
						curHp = m_lastPlayerHl2Hp;
						ExecCmd("play weapons/fx/rics/ric1.wav");
					} else {
						if (curHp <= 0) {
							// Player died in HL2 -> clear blocks and kill props
							m_collisionEpoch++;
							m_placedBlocks.clear();
							D3D9WorldRender::Get().ClearSections();
							ExecCmd("ent_fire mc_blk_* Kill;");
							uint64_t head = *(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff);
							uint64_t ringPos = head % proto::kColRingDataBytes;
							uint8_t *slot = m_shmView + proto::kOffCollisionRing + proto::kColRingDataOff + ringPos;
							*(uint32_t*)slot = proto::kColClear;
							*(uint32_t*)(slot + 4) = 4;
							*(uint32_t*)(slot + 8) = m_collisionEpoch;
							MemoryBarrier();
							*(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff) = head + 16;
						}
						PushInput(proto::kInHurt, 0, dmg * 20, 0);
					}
				}
				m_lastPlayerHl2Hp = curHp;
			}
		}

		// Synchronize death from Minecraft
		bool mcDead = (ms->flags & proto::kMcDead) != 0;
		if (mcDead && !m_mcWasDead) {
			m_mcWasDead = true;
			m_placedBlocks.clear();
			D3D9WorldRender::Get().ClearSections();
			ExecCmd("ent_fire mc_blk_* Kill; ent_fire !player sethealth 0");
		} else if (!mcDead && m_mcWasDead) {
			m_mcWasDead = false;
			m_lastSyncedHp = 100;
			m_lastPlayerHl2Hp = 100;
			ExecCmd("respawn; ent_fire !player sethealth 100");
		}

		// Synchronize health from Minecraft (if alive)
		if (!mcDead) {
			int targetHp = (int)std::ceil(ms->health * 5.0f);
			if (targetHp < 1) targetHp = 1;
			if (targetHp > 100) targetHp = 100;
			if (targetHp != m_lastSyncedHp) {
				m_lastSyncedHp = targetHp;
				m_lastPlayerHl2Hp = targetHp;
				char cmd[64];
				snprintf(cmd, sizeof(cmd), "ent_fire !player sethealth %d", targetHp);
				ExecCmd(cmd);
			}
		}

		m_creative = (ms->gameMode == 1);
	}

	static bool IsNpcClass(const char *className)
	{
		if (!className || !className[0]) return false;
		if (strstr(className, "NPC") != nullptr) return true;
		if (strstr(className, "Zombie") != nullptr) return true;
		if (strstr(className, "Headcrab") != nullptr) return true;
		if (strstr(className, "Antlion") != nullptr) return true;
		if (strstr(className, "Vortigaunt") != nullptr) return true;
		if (strstr(className, "Combine") != nullptr) return true;
		if (strstr(className, "Barnacle") != nullptr) return true;
		if (strstr(className, "Manhack") != nullptr) return true;
		if (strstr(className, "Scanner") != nullptr) return true;
		if (strstr(className, "Strider") != nullptr) return true;
		if (strstr(className, "Gunship") != nullptr) return true;
		if (strstr(className, "Helicopter") != nullptr) return true;
		if (strstr(className, "Rollermine") != nullptr) return true;
		if (strstr(className, "Stalker") != nullptr) return true;
		return false;
	}

	static bool IsPropClass(const char *className)
	{
		if (!className || !className[0]) return false;
		if (strstr(className, "Prop") != nullptr) return true;
		if (strstr(className, "Physics") != nullptr) return true;
		if (strstr(className, "Breakable") != nullptr) return true;
		return false;
	}

	class CTraceFilterEnvironment : public ITraceFilter {
	public:
		void *pPlayer = nullptr;
		virtual bool ShouldHitEntity(void *pEntity, int contentsMask) {
			if (!pEntity || pEntity == pPlayer) return false;
			void *networkable = (char*)pEntity + 8;
			if (!IsBadReadPtr(networkable, sizeof(void*))) {
				void **netVt = *(void***)networkable;
				if (!IsBadReadPtr(netVt, sizeof(void*) * 3)) {
					typedef void* (__thiscall *GetClientClassFn)(void*);
					void *clientClass = ((GetClientClassFn)netVt[2])(networkable);
					if (clientClass && !IsBadReadPtr(clientClass, sizeof(void*) * 3)) {
						const char *className = *(const char**)((char*)clientClass + 8);
						if (IsNpcClass(className)) return false;
					}
				}
			}
			return true;
		}
		virtual int GetTraceType() const { return 0; } // TRACE_EVERYTHING
	};

	class CTraceFilterCombat : public ITraceFilter {
	public:
		void *pPlayer = nullptr;
		virtual bool ShouldHitEntity(void *pEntity, int contentsMask) {
			if (!pEntity || pEntity == pPlayer) return false;
			return true;
		}
		virtual int GetTraceType() const { return 0; } // TRACE_EVERYTHING
	};

	typedef BOOL (WINAPI *SetCursorPosFn)(int, int);
	static SetCursorPosFn g_realSetCursorPos = nullptr;
	static uint8_t g_savedCursorBytes[5] = {0};

	static BOOL WINAPI Hook_SetCursorPos(int x, int y) {
		if (HL2Bridge::Get().IsGuiOpen()) {
			return TRUE; // Do not center or move cursor while inventory or chat is open
		}
		DWORD oldProt;
		VirtualProtect((void*)g_realSetCursorPos, 5, PAGE_EXECUTE_READWRITE, &oldProt);
		std::memcpy((void*)g_realSetCursorPos, g_savedCursorBytes, 5);
		BOOL res = g_realSetCursorPos(x, y);
		uint8_t jmp[5] = { 0xE9, 0, 0, 0, 0 };
		*(int32_t*)(jmp + 1) = (int32_t)((char*)Hook_SetCursorPos - (char*)g_realSetCursorPos - 5);
		std::memcpy((void*)g_realSetCursorPos, jmp, 5);
		VirtualProtect((void*)g_realSetCursorPos, 5, oldProt, &oldProt);
		return res;
	}

	void HL2Bridge::ScanCollision(const Vector &playerPos)
	{
		if (!m_engineTrace || !m_shmView || !IsInPlayableWorld()) return;
		void **vt = *(void***)m_engineTrace;
		if (!vt || !vt[4] || !vt[0]) return;
		typedef void (__thiscall *TraceRayFn)(void*, const Ray_t&, unsigned int, ITraceFilter*, trace_t*);
		TraceRayFn traceRay = (TraceRayFn)vt[4];
		typedef int (__thiscall *GetPointContentsFn)(void*, const Vector&, void**);
		GetPointContentsFn getPointContents = (GetPointContentsFn)vt[0];

		CTraceFilterEnvironment filter;
		if (m_entityList && m_engineClient) {
			void **evt = *(void***)m_engineClient;
			typedef int (__thiscall *GetLocalPlayerFn)(void*);
			int localPlayer = ((GetLocalPlayerFn)evt[12])(m_engineClient);
			if (localPlayer > 0) {
				void **lvt = *(void***)m_entityList;
				typedef void* (__thiscall *GetEntityFn)(void*, int);
				filter.pPlayer = ((GetEntityFn)lvt[3])(m_entityList, localPlayer);
			}
		}
		constexpr int kRadius = 26; // 26 blocks horizontally around player (53x53 grid = ~53 meters)
		constexpr int kGridSize = kRadius * 2 + 1;
		constexpr float kBlockUnits = (float)proto::kUnitsPerBlock;

		constexpr double kYOffset = 64.0;
		int centerBx = (int)std::floor(playerPos.x * proto::kMetersPerUnit);
		int centerBz = (int)std::floor(-playerPos.y * proto::kMetersPerUnit);
		int centerBy = (int)std::floor((playerPos.z * proto::kMetersPerUnit) + kYOffset);

		int minX = centerBx - kRadius, maxX = centerBx + kRadius;
		int minZ = centerBz - kRadius, maxZ = centerBz + kRadius;
		int minY = centerBy - 14, maxY = centerBy + 14;

		std::vector<proto::ColBlock> blocks;
		blocks.reserve(kGridSize * kGridSize * 4);

		int floorGrid[kGridSize][kGridSize];
		float floorZGrid[kGridSize][kGridSize];
		for (int x = 0; x < kGridSize; x++) {
			for (int z = 0; z < kGridSize; z++) {
				floorGrid[x][z] = -9999;
				floorZGrid[x][z] = 0.0f;
			}
		}

		g_inTrace = true;
		if (setjmp(g_traceJmp) != 0) {
			g_inTrace = false;
			return; // Safely handled any engine trace exception
		}

		constexpr unsigned int kStaticWorldMask = 0xB; // CONTENTS_SOLID | CONTENTS_WINDOW | CONTENTS_GRATE (no props or NPCs)

		// Pass 1: Floor detection
		for (int ix = 0; ix < kGridSize; ix++) {
			int bx = minX + ix;
			for (int iz = 0; iz < kGridSize; iz++) {
				int bz = minZ + iz;

				float hx = (bx + 0.5f) * kBlockUnits;
				float hy = -(bz + 0.5f) * kBlockUnits;
				float hzStart = playerPos.z + 42.0f;
				float hzEnd = playerPos.z - 420.0f;

				Ray_t ray;
				ray.Init(Vector(hx, hy, hzStart), Vector(hx, hy, hzEnd));
				trace_t tr;
				std::memset(&tr, 0, sizeof(tr));
				traceRay(m_engineTrace, ray, kStaticWorldMask, &filter, &tr);

				if (tr.startsolid) {
					// Step-up or hill: try starting slightly higher (below doorway height) if that point is in open air
					float hzHigh = playerPos.z + 82.0f;
					if (!getPointContents || (getPointContents(m_engineTrace, Vector(hx, hy, hzHigh), nullptr) & kStaticWorldMask) == 0) {
						ray.Init(Vector(hx, hy, hzHigh), Vector(hx, hy, hzEnd));
						std::memset(&tr, 0, sizeof(tr));
						traceRay(m_engineTrace, ray, kStaticWorldMask, &filter, &tr);
					}
				}

				if (tr.fraction < 1.0f && !tr.startsolid) {
					float floorZ = tr.endpos.z;
					float floorYMc = (floorZ * (float)proto::kMetersPerUnit) + (float)kYOffset;
					int by = (int)std::floor(floorYMc - 0.20f);

					if (by >= minY && by <= maxY) {
						floorGrid[ix][iz] = by;
						floorZGrid[ix][iz] = floorZ;

						proto::ColBlock topBlock;
						topBlock.x = bx;
						topBlock.y = by;
						topBlock.z = bz;
						topBlock.pad = 0;
						int sub = std::clamp((int)((floorYMc - by) * 8.0f), 0, 7);
						for (int l = 0; l < 8; l++) {
							topBlock.bits[l] = (l <= sub) ? 0xFFFFFFFFFFFFFFFFULL : 0ULL;
						}
						blocks.push_back(topBlock);

						// One solid support block directly below floor surface
						proto::ColBlock subBlock;
						subBlock.x = bx;
						subBlock.y = by - 1;
						subBlock.z = bz;
						subBlock.pad = 0;
						for (int l = 0; l < 8; l++) subBlock.bits[l] = 0xFFFFFFFFFFFFFFFFULL;
						blocks.push_back(subBlock);
					}
				}
			}
		}

		// Pass 2: Actual solid brush walls (verified via getPointContents, never guessed from height steps)
		if (getPointContents) {
			for (int ix = 0; ix < kGridSize; ix++) {
				int bx = minX + ix;
				for (int iz = 0; iz < kGridSize; iz++) {
					int bz = minZ + iz;
					int by = floorGrid[ix][iz];
					int baseWy = (by != -9999) ? (by + 1) : (centerBy - 1);
					int topWy = std::min(baseWy + 4, maxY);

					for (int wy = baseWy; wy <= topWy; wy++) {
						float cx = (bx + 0.5f) * kBlockUnits;
						float cy = -(bz + 0.5f) * kBlockUnits;
						float cz = (wy + 0.5f - (float)kYOffset) * kBlockUnits;
						if ((getPointContents(m_engineTrace, Vector(cx, cy, cz), nullptr) & kStaticWorldMask) != 0) {
							proto::ColBlock wb;
							wb.x = bx; wb.y = wy; wb.z = bz; wb.pad = 0;
							for (int l = 0; l < 8; l++) wb.bits[l] = 0xFFFFFFFFFFFFFFFFULL;
							blocks.push_back(wb);
						}
					}
				}
			}
		}

		g_inTrace = false;

		if (blocks.empty()) return;

		// Write to CollisionRing
		uint64_t head = *(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff);
		uint32_t payloadBytes = sizeof(proto::ColRegion) + blocks.size() * sizeof(proto::ColBlock);
		uint32_t totalMsgBytes = (8 + payloadBytes + 7) & ~7;

		uint64_t pos = head % proto::kColRingDataBytes;
		if (pos + totalMsgBytes > proto::kColRingDataBytes) {
			// Wrap pad
			uint8_t *padSlot = m_shmView + proto::kOffCollisionRing + proto::kColRingDataOff + pos;
			*(uint32_t*)padSlot = proto::kColPad;
			*(uint32_t*)(padSlot + 4) = 0;
			head += (proto::kColRingDataBytes - pos);
			pos = 0;
		}

		uint8_t *slot = m_shmView + proto::kOffCollisionRing + proto::kColRingDataOff + pos;
		*(uint32_t*)slot = proto::kColRegion;
		*(uint32_t*)(slot + 4) = payloadBytes;

		proto::ColRegion *reg = (proto::ColRegion*)(slot + 8);
		reg->minX = minX; reg->minY = minY; reg->minZ = minZ;
		reg->maxX = maxX; reg->maxY = maxY; reg->maxZ = maxZ;
		reg->epoch = m_collisionEpoch;
		reg->count = (uint32_t)blocks.size();

		std::memcpy(slot + 8 + sizeof(proto::ColRegion), blocks.data(), blocks.size() * sizeof(proto::ColBlock));
		MemoryBarrier();
		*(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff) = head + totalMsgBytes;
	}

	void HL2Bridge::ScanActors()
	{
		if (!m_entityList || !m_shmView || !IsInPlayableWorld()) return;
		void **vt = *(void***)m_entityList;
		typedef void* (__thiscall *GetEntityFn)(void*, int);
		typedef int (__thiscall *GetHighestFn)(void*);
		GetEntityFn getEntity = (GetEntityFn)vt[3];
		GetHighestFn getHighest = (GetHighestFn)vt[6];

		int localPlayer = 1;
		if (m_engineClient) {
			void **evt = *(void***)m_engineClient;
			typedef int (__thiscall *GetLocalPlayerFn)(void*);
			localPlayer = ((GetLocalPlayerFn)evt[12])(m_engineClient);
		}

		int maxEnts = getHighest ? getHighest(m_entityList) : 64;
		if (maxEnts > 128) maxEnts = 128;

		proto::ActorTable *tbl = (proto::ActorTable*)(m_shmView + proto::kOffActorTable);
		uint32_t seq = tbl->seq;
		tbl->seq = seq + 1;
		MemoryBarrier();

		constexpr double kYOffset = 64.0;
		uint32_t count = 0;
		for (int i = 2; i <= maxEnts && count < proto::kMaxActors; i++) {
			if (i == localPlayer) continue;

			void *ent = getEntity(m_entityList, i);
			if (!ent || IsBadReadPtr(ent, sizeof(void*))) continue;

			void *networkable = (char*)ent + 8;
			if (IsBadReadPtr(networkable, sizeof(void*))) continue;
			void **netVt = *(void***)networkable;
			if (IsBadReadPtr(netVt, sizeof(void*) * 9)) continue;

			typedef void* (__thiscall *GetClientClassFn)(void*);
			typedef bool (__thiscall *IsDormantFn)(void*);
			void *clientClass = ((GetClientClassFn)netVt[2])(networkable);
			if (!clientClass || IsBadReadPtr(clientClass, sizeof(void*) * 3)) continue;
			const char *className = *(const char**)((char*)clientClass + 8);
			if (!className) continue;

			bool isNpc = IsNpcClass(className);
			bool isProp = IsPropClass(className);
			if (!isNpc && !isProp) continue;

			if (((IsDormantFn)netVt[8])(networkable)) continue;

			if (isNpc) {
				if (!IsBadReadPtr((char*)ent + 0x8F, 8)) {
					uint8_t lifeState = *(uint8_t*)((char*)ent + 0x8F);
					int health = *(int*)((char*)ent + 0x90);
					if (lifeState != 0 || health <= 0) continue;
				}
			}

			void *renderable = (char*)ent + 4;
			if (IsBadReadPtr(renderable, sizeof(void*))) continue;
			void **rendVt = *(void***)renderable;
			if (IsBadReadPtr(rendVt, sizeof(void*) * 2)) continue;

			typedef const float* (__thiscall *GetOriginFn)(void*);
			const float *org = ((GetOriginFn)rendVt[1])(renderable);
			if (!org || IsBadReadPtr(org, sizeof(float) * 3)) continue;

			proto::ActorRecord &rec = tbl->actors[count];
			rec.entIndex = i;
			rec.flags = isNpc ? proto::kActorHostile : 0;
			rec.x = org[0] * (float)proto::kMetersPerUnit;
			rec.y = (org[2] * (float)proto::kMetersPerUnit) + (float)kYOffset;
			rec.z = -org[1] * (float)proto::kMetersPerUnit;
			rec.yaw = 0.0f;

			float w = 0.6f;
			float h = 1.8f;
			if (isProp) {
				w = 0.7f; h = 0.8f;
			} else if (strstr(className, "Headcrab")) {
				w = 0.4f; h = 0.4f;
			} else if (strstr(className, "Zombie")) {
				w = 0.7f; h = 1.9f;
			} else if (strstr(className, "AntlionGuard")) {
				w = 2.0f; h = 2.5f;
			}
			rec.width = w;
			rec.height = h;
			rec.healthFrac = 1.0f;
			rec.classId = 0;
			rec.name[0] = '\0';
			count++;
		}

		tbl->count = count;
		MemoryBarrier();
		tbl->seq = seq + 2;
	}

	void HL2Bridge::ProcessEvents()
	{
		if (!m_shmView || !IsInPlayableWorld()) return;
		uint64_t head = *(uint64_t*)(m_shmView + proto::kOffEventRing + proto::kEventRingHeadOff);
		uint64_t tail = *(uint64_t*)(m_shmView + proto::kOffEventRing + proto::kEventRingTailOff);
		if (tail >= head) return;

		static void *s_engineServer = nullptr;
		static void *s_serverEnts = nullptr;
		if (!s_engineServer) {
			HMODULE hEngine = GetModuleHandleA("engine.dll");
			if (hEngine) {
				CreateInterfaceFn engineFactory = (CreateInterfaceFn)GetProcAddress(hEngine, "CreateInterface");
				if (engineFactory) s_engineServer = engineFactory("VEngineServer021", nullptr);
			}
		}
		if (!s_serverEnts) {
			HMODULE hServer = GetModuleHandleA("server.dll");
			if (hServer) {
				CreateInterfaceFn serverFactory = (CreateInterfaceFn)GetProcAddress(hServer, "CreateInterface");
				if (serverFactory) s_serverEnts = serverFactory("ServerGameEnts001", nullptr);
			}
		}

		while (tail < head) {
			uint64_t off = proto::kOffEventRing + proto::kEventRingDataOff + (tail % proto::kEventRingEntries) * sizeof(proto::McEvent);
			proto::McEvent *ev = (proto::McEvent*)(m_shmView + off);
			if (ev->type == proto::kEvHitActor) {
				int targetEnt = ev->entIndex;
				int dmg = (int)std::max(ev->a * 3.5f, 25.0f);
				Vector entPos{0, 0, 0};
				bool foundPos = false;
				if (targetEnt > 0 && m_entityList) {
					void **evt = *(void***)m_entityList;
					typedef void* (__thiscall *GetClientEntityFn)(void*, int);
					GetClientEntityFn getEntity = (GetClientEntityFn)evt[3];
					void *ent = getEntity(m_entityList, targetEnt);
					if (ent && !IsBadReadPtr(ent, sizeof(void*))) {
						void *renderable = (char*)ent + 4;
						if (!IsBadReadPtr(renderable, sizeof(void*))) {
							void **rendVt = *(void***)renderable;
							if (!IsBadReadPtr(rendVt, sizeof(void*) * 2)) {
								typedef const float* (__thiscall *GetOriginFn)(void*);
								const float *org = ((GetOriginFn)rendVt[1])(renderable);
								if (org && !IsBadReadPtr(org, sizeof(float) * 3)) {
									entPos = Vector{ org[0], org[1], org[2] };
									foundPos = true;
								}
							}
						}
					}
				}

				if (foundPos) {
					static uint32_t s_hurtCounter = 0;
					uint32_t hurtId = ++s_hurtCounter;
					char cmd[320];
					snprintf(cmd, sizeof(cmd),
						"ent_create point_hurt targetname mc_hrt_%u; "
						"ent_fire mc_hrt_%u addoutput \"origin %.2f %.2f %.2f\"; "
						"ent_fire mc_hrt_%u addoutput \"Damage %d\"; "
						"ent_fire mc_hrt_%u addoutput \"DamageRadius 50\"; "
						"ent_fire mc_hrt_%u addoutput \"DamageType 2\"; "
						"ent_fire mc_hrt_%u Hurt; "
						"ent_fire mc_hrt_%u Kill",
						hurtId, hurtId, entPos.x, entPos.y, entPos.z + 32.0f,
						hurtId, dmg, hurtId, hurtId, hurtId, hurtId);
					ExecCmd(cmd);
				} else {
					HitCurrentTarget(dmg);
				}
			} else if (ev->type == proto::kEvExplosion) {
				float sx = ev->a * (float)proto::kUnitsPerBlock;
				float sz = (ev->b - 64.0f) * (float)proto::kUnitsPerBlock;
				float sy = -ev->c * (float)proto::kUnitsPerBlock;
				int mag = (int)std::max(ev->d * 40.0f, 150.0f);
				float radius = std::max(ev->d * 60.0f, 300.0f);

				// Kill collision props for any placed blocks within explosion radius
				float expR = ev->d + 1.5f;
				float expRSq = expR * expR;
				std::vector<std::tuple<int, int, int>> toRemove;
				for (const auto &bPos : m_placedBlocks) {
					float dx = (std::get<0>(bPos) + 0.5f) - ev->a;
					float dy = (std::get<1>(bPos) + 0.5f) - ev->b;
					float dz = (std::get<2>(bPos) + 0.5f) - ev->c;
					if (dx * dx + dy * dy + dz * dz <= expRSq) {
						toRemove.push_back(bPos);
					}
				}
				for (const auto &bPos : toRemove) {
					int bx = std::get<0>(bPos);
					int by = std::get<1>(bPos);
					int bz = std::get<2>(bPos);
					char delCmd[128];
					snprintf(delCmd, sizeof(delCmd), "ent_fire mc_blk_%d_%d_%d Kill;", bx, by, bz);
					ExecCmd(delCmd);
					m_placedBlocks.erase(bPos);
				}

				static uint32_t s_expCounter = 0;
				uint32_t expId = ++s_expCounter;
				char cmd[320];
				snprintf(cmd, sizeof(cmd),
					"ent_create env_explosion targetname mc_exp_%u; "
					"ent_fire mc_exp_%u addoutput \"origin %.2f %.2f %.2f\"; "
					"ent_fire mc_exp_%u addoutput \"iMagnitude %d\"; "
					"ent_fire mc_exp_%u addoutput \"iRadiusOverride %.1f\"; "
					"ent_fire mc_exp_%u Explode; "
					"ent_fire mc_exp_%u Kill",
					expId, expId, sx, sy, sz, expId, mag, expId, radius, expId, expId);
				ExecCmd(cmd);
			} else if (ev->type == proto::kEvTeleport) {
				float sx = ev->a * (float)proto::kUnitsPerBlock;
				float sz = (ev->b - 64.0f) * (float)proto::kUnitsPerBlock + 16.0f;
				float sy = -ev->c * (float)proto::kUnitsPerBlock;

				char cmd[128];
				snprintf(cmd, sizeof(cmd), "setpos %.2f %.2f %.2f", sx, sy, sz);
				ExecCmd(cmd);

				m_lastPosX = sx;
				m_lastPosY = sy;
				m_lastScanPos = Vector{ sx, sy, sz };
				m_teleportSeq++;

				auto *hs = (proto::HostState*)(m_shmView + proto::kOffHostState);
				if (hs) {
					hs->posX = ev->a;
					hs->posY = ev->b + 1.0;
					hs->posZ = ev->c;
					hs->teleportSeq = m_teleportSeq;
				}
			} else if (ev->type == proto::kEvPlayerDied) {
				m_placedBlocks.clear();
				ExecCmd("ent_fire mc_blk_* Kill; kill");
			} else if (ev->type == proto::kEvClearBlocks) {
				m_placedBlocks.clear();
				ExecCmd("ent_fire mc_blk_* Kill");
			} else if (ev->type == proto::kEvBlockPlaced) {
				float sx = (ev->a + 0.5f) * (float)proto::kUnitsPerBlock;
				float sy = -(ev->c + 0.5f) * (float)proto::kUnitsPerBlock;
				float sz = (ev->b + 0.5f - 64.0f) * (float)proto::kUnitsPerBlock;
				int ibx = (int)std::round(ev->a);
				int iby = (int)std::round(ev->b);
				int ibz = (int)std::round(ev->c);
				m_placedBlocks.insert({ibx, iby, ibz});

				// Block AABB in Source coordinates
				float bMinX = (float)ibx * (float)proto::kUnitsPerBlock;
				float bMaxX = ((float)ibx + 1.0f) * (float)proto::kUnitsPerBlock;
				float bMinY = -((float)ibz + 1.0f) * (float)proto::kUnitsPerBlock;
				float bMaxY = -(float)ibz * (float)proto::kUnitsPerBlock;
				float bMinZ = ((float)iby - 64.0f) * (float)proto::kUnitsPerBlock;
				float bMaxZ = ((float)iby + 1.0f - 64.0f) * (float)proto::kUnitsPerBlock;

				Vector playerPos{0, 0, 0};
				bool hasPlayer = GetPlayerPos(playerPos);
				bool overlapping = false;
				Vector targetPos = playerPos;

				if (hasPlayer) {
					// Player hull (+/-16.0 horizontally, 0..72 vertically)
					float pMinX = playerPos.x - 16.0f;
					float pMaxX = playerPos.x + 16.0f;
					float pMinY = playerPos.y - 16.0f;
					float pMaxY = playerPos.y + 16.0f;
					float pMinZ = playerPos.z + 0.5f;
					float pMaxZ = playerPos.z + 71.5f;

					bool overlapX = (pMinX < bMaxX) && (pMaxX > bMinX);
					bool overlapY = (pMinY < bMaxY) && (pMaxY > bMinY);
					bool overlapZ = (pMinZ < bMaxZ) && (pMaxZ > bMinZ);

					if (overlapX && overlapY && overlapZ) {
						overlapping = true;
						ComputeUnstickPos(playerPos, ibx, iby, ibz, targetPos);
					}
				}

				char cmd[1024];
				if (overlapping) {
					snprintf(cmd, sizeof(cmd),
						"setpos %.2f %.2f %.2f; "
						"ent_create prop_physics_override model models/props_junk/wood_crate001a.mdl targetname mc_blk_%d_%d_%d disableshadows 1 disablereceiveshadows 1 rendermode 10 modelscale 0.99; "
						"ent_fire mc_blk_%d_%d_%d addoutput \"origin %.2f %.2f %.2f\"; "
						"ent_fire mc_blk_%d_%d_%d addoutput \"angles 0 0 0\"; "
						"ent_fire mc_blk_%d_%d_%d addoutput \"rendermode 10\"; "
						"ent_fire mc_blk_%d_%d_%d addoutput \"effects 80\"; "
						"ent_fire mc_blk_%d_%d_%d DisableShadow; "
						"ent_fire mc_blk_%d_%d_%d addoutput \"health 999999\"; "
						"ent_fire mc_blk_%d_%d_%d DisableMotion; "
						"setpos %.2f %.2f %.2f",
						targetPos.x, targetPos.y, targetPos.z,
						ibx, iby, ibz,
						ibx, iby, ibz, sx, sy, sz,
						ibx, iby, ibz,
						ibx, iby, ibz,
						ibx, iby, ibz,
						ibx, iby, ibz,
						ibx, iby, ibz,
						ibx, iby, ibz,
						targetPos.x, targetPos.y, targetPos.z);
				} else {
					snprintf(cmd, sizeof(cmd),
						"ent_create prop_physics_override model models/props_junk/wood_crate001a.mdl targetname mc_blk_%d_%d_%d disableshadows 1 disablereceiveshadows 1 rendermode 10 modelscale 0.99; "
						"ent_fire mc_blk_%d_%d_%d addoutput \"origin %.2f %.2f %.2f\"; "
						"ent_fire mc_blk_%d_%d_%d addoutput \"angles 0 0 0\"; "
						"ent_fire mc_blk_%d_%d_%d addoutput \"rendermode 10\"; "
						"ent_fire mc_blk_%d_%d_%d addoutput \"effects 80\"; "
						"ent_fire mc_blk_%d_%d_%d DisableShadow; "
						"ent_fire mc_blk_%d_%d_%d addoutput \"health 999999\"; "
						"ent_fire mc_blk_%d_%d_%d DisableMotion",
						ibx, iby, ibz,
						ibx, iby, ibz, sx, sy, sz,
						ibx, iby, ibz,
						ibx, iby, ibz,
						ibx, iby, ibz,
						ibx, iby, ibz,
						ibx, iby, ibz,
						ibx, iby, ibz);
				}
				ExecCmd(cmd);
			} else if (ev->type == proto::kEvBlockBroken) {
				int ibx = (int)std::round(ev->a);
				int iby = (int)std::round(ev->b);
				int ibz = (int)std::round(ev->c);
				m_placedBlocks.erase({ibx, iby, ibz});
				char cmd[128];
				snprintf(cmd, sizeof(cmd), "ent_fire mc_blk_%d_%d_%d Kill", ibx, iby, ibz);
				ExecCmd(cmd);
			}
			tail++;
		}
		*(uint64_t*)(m_shmView + proto::kOffEventRing + proto::kEventRingTailOff) = tail;
	}

	void HL2Bridge::HitPicker(int extraDmg)
	{
	}

	void HL2Bridge::HitCurrentTarget(int dmg)
	{
		if (!m_engineTrace || !IsInPlayableWorld()) return;
		Vector pos{0, 0, 0};
		QAngle angles{0, 0, 0};
		if (!GetPlayerPos(pos) || !GetAngles(angles)) return;

		float radYaw = angles.yaw * 3.14159265f / 180.0f;
		float radPitch = -angles.pitch * 3.14159265f / 180.0f;
		Vector fwd;
		fwd.x = std::cos(radPitch) * std::cos(radYaw);
		fwd.y = std::cos(radPitch) * std::sin(radYaw);
		fwd.z = std::sin(radPitch);

		Vector start = pos;
		start.z += 64.0f;
		Vector end;
		end.x = start.x + fwd.x * 95.0f;
		end.y = start.y + fwd.y * 95.0f;
		end.z = start.z + fwd.z * 95.0f;

		Ray_t ray;
		ray.Init(start, end);
		trace_t tr;
		std::memset(&tr, 0, sizeof(tr));
		CTraceFilterCombat filter;
		if (m_entityList && m_engineClient) {
			void **evt = *(void***)m_engineClient;
			typedef int (__thiscall *GetLocalPlayerFn)(void*);
			int localPlayer = ((GetLocalPlayerFn)evt[12])(m_engineClient);
			if (localPlayer > 0) {
				void **lvt = *(void***)m_entityList;
				typedef void* (__thiscall *GetEntityFn)(void*, int);
				filter.pPlayer = ((GetEntityFn)lvt[3])(m_entityList, localPlayer);
			}
		}

		void **vt = *(void***)m_engineTrace;
		typedef void (__thiscall *TraceRayFn)(void*, const Ray_t&, unsigned int, ITraceFilter*, trace_t*);
		((TraceRayFn)vt[4])(m_engineTrace, ray, 0x200400B, &filter, &tr);

		if (tr.fraction < 1.0f && tr.m_pEnt) {
			void *networkable = (char*)tr.m_pEnt + 8;
			const char *className = "";
			if (!IsBadReadPtr(networkable, sizeof(void*))) {
				void **netVt = *(void***)networkable;
				if (!IsBadReadPtr(netVt, sizeof(void*) * 3)) {
					typedef void* (__thiscall *GetClientClassFn)(void*);
					void *clientClass = ((GetClientClassFn)netVt[2])(networkable);
					if (clientClass && !IsBadReadPtr(clientClass, sizeof(void*) * 3)) {
						className = *(const char**)((char*)clientClass + 8);
					}
				}
			}

			bool isNpc = IsNpcClass(className);
			bool isProp = IsPropClass(className);
			if (isNpc) {
				if (!IsBadReadPtr((char*)tr.m_pEnt + 0x90, 4)) {
					int curHp = *(int*)((char*)tr.m_pEnt + 0x90);
					int newHp = std::max(0, curHp - dmg);
					char cmd[64];
					if (newHp <= 0) {
						snprintf(cmd, sizeof(cmd), "ent_fire !picker sethealth 0; ent_fire !picker Die");
					} else {
						snprintf(cmd, sizeof(cmd), "ent_fire !picker sethealth %d", newHp);
					}
					ExecCmd(cmd);
				}
			} else if (isProp && m_creative) {
				ExecCmd("ent_fire !picker Break");
			}
		}
	}

	bool HL2Bridge::TraceWorldRay(const Vector &start, const Vector &end, trace_t &tr)
	{
		if (!m_engineTrace || !IsInPlayableWorld()) return false;
		void **vt = *(void***)m_engineTrace;
		if (!vt || !vt[4]) return false;
		typedef void (__thiscall *TraceRayFn)(void*, const Ray_t&, unsigned int, ITraceFilter*, trace_t*);
		TraceRayFn traceRay = (TraceRayFn)vt[4];

		CTraceFilterWorldOnly filter;
		Ray_t ray;
		ray.Init(start, end);
		std::memset(&tr, 0, sizeof(tr));
		traceRay(m_engineTrace, ray, 0x200400B, &filter, &tr);
		return true;
	}

	void HL2Bridge::ComputeUnstickPos(const Vector &playerPos, int ibx, int iby, int ibz, Vector &outPos)
	{
		float bMinX = (float)ibx * (float)proto::kUnitsPerBlock;
		float bMaxX = ((float)ibx + 1.0f) * (float)proto::kUnitsPerBlock;
		float bMinY = -((float)ibz + 1.0f) * (float)proto::kUnitsPerBlock;
		float bMaxY = -(float)ibz * (float)proto::kUnitsPerBlock;
		float bMinZ = ((float)iby - 64.0f) * (float)proto::kUnitsPerBlock;
		float bMaxZ = ((float)iby + 1.0f - 64.0f) * (float)proto::kUnitsPerBlock;

		// 1. Always try pushing UP first (if the 2 blocks above and HL2 ceiling are clear)
		bool upClear = (m_placedBlocks.find({ibx, iby + 1, ibz}) == m_placedBlocks.end()) &&
		               (m_placedBlocks.find({ibx, iby + 2, ibz}) == m_placedBlocks.end());
		if (upClear) {
			trace_t tr;
			Vector upStart{ playerPos.x, playerPos.y, std::max(playerPos.z + 18.0f, bMaxZ - 8.0f) };
			Vector upEnd{ playerPos.x, playerPos.y, bMaxZ + 73.0f };
			if (TraceWorldRay(upStart, upEnd, tr) && (tr.startsolid || tr.fraction < 0.98f)) {
				upClear = false;
			}
		}

		if (upClear) {
			outPos.x = playerPos.x;
			outPos.y = playerPos.y;
			outPos.z = bMaxZ + 1.0f;
			return;
		}

		// 2. Pushing UP is blocked -> push horizontally to the nearest free side
		struct SideCandidate {
			float x, y;
			float traceEndX, traceEndY;
			int nbx, nbz;
			float dist;
		};
		SideCandidate cands[4] = {
			{ bMinX - 17.5f, playerPos.y, bMinX - 33.0f, playerPos.y, ibx - 1, ibz, std::abs((bMinX - 17.5f) - playerPos.x) },
			{ bMaxX + 17.5f, playerPos.y, bMaxX + 33.0f, playerPos.y, ibx + 1, ibz, std::abs((bMaxX + 17.5f) - playerPos.x) },
			{ playerPos.x, bMinY - 17.5f, playerPos.x, bMinY - 33.0f, ibx, ibz + 1, std::abs((bMinY - 17.5f) - playerPos.y) },
			{ playerPos.x, bMaxY + 17.5f, playerPos.x, bMaxY + 33.0f, ibx, ibz - 1, std::abs((bMaxY + 17.5f) - playerPos.y) }
		};
		std::sort(cands, cands + 4, [](const SideCandidate &a, const SideCandidate &b) {
			return a.dist < b.dist;
		});

		int footBy = (int)std::floor(playerPos.z / (float)proto::kUnitsPerBlock + 64.0f + 0.1f);
		float centerBx = (bMinX + bMaxX) * 0.5f;
		float centerBy = (bMinY + bMaxY) * 0.5f;
		float traceZ = playerPos.z + 36.0f;

		int chosenIdx = 0;
		for (int i = 0; i < 4; ++i) {
			bool sideBlocked = (m_placedBlocks.find({cands[i].nbx, footBy, cands[i].nbz}) != m_placedBlocks.end()) ||
			                   (m_placedBlocks.find({cands[i].nbx, footBy + 1, cands[i].nbz}) != m_placedBlocks.end());
			if (!sideBlocked) {
				trace_t tr;
				Vector sStart{ centerBx, centerBy, traceZ };
				Vector sEnd{ cands[i].traceEndX, cands[i].traceEndY, traceZ };
				if (TraceWorldRay(sStart, sEnd, tr) && (tr.startsolid || tr.fraction < 0.95f)) {
					sideBlocked = true;
				}
			}
			if (!sideBlocked) {
				chosenIdx = i;
				break;
			}
		}

		outPos.x = cands[chosenIdx].x;
		outPos.y = cands[chosenIdx].y;
		outPos.z = playerPos.z;
	}

	void HL2Bridge::ProcessInput()
	{
		HWND fgWnd = GetForegroundWindow();
		DWORD fgPid = 0;
		if (fgWnd) GetWindowThreadProcessId(fgWnd, &fgPid);
		if (fgPid != GetCurrentProcessId()) return;

		if (m_guiOpen) {
			ExecCmd("-forward; -back; -moveleft; -moveright; -jump; -duck; -speed; -attack; -attack2;");
			return; // Frozen while GUI is open!
		}

		if (!m_shmView) return;

		bool sprinting = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		if (sprinting != m_lastSprinting) {
			m_lastSprinting = sprinting;
			ExecCmd(sprinting ? "hl2_normspeed 320" : "hl2_normspeed 190");
		}

		bool lbtn = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		if (lbtn != m_lastLButton) {
			m_lastLButton = lbtn;
			PushInput(proto::kInMouseButton, 1, lbtn ? 1 : 0);
			if (lbtn) {
				HitCurrentTarget(35);
			}
		}
		bool rbtn = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
		if (rbtn != m_lastRButton) {
			m_lastRButton = rbtn;
			PushInput(proto::kInMouseButton, 3, rbtn ? 1 : 0);
		}

		auto handleKey = [&](int vk, uint16_t scancode) {
			bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			uint8_t idx = (uint8_t)(vk & 0xFF);
			if (down != m_keysDown[idx]) {
				m_keysDown[idx] = down;
				PushInput(proto::kInKey, scancode, down ? 1 : 0);
			}
		};

		handleKey('E', 8);          // 8 = 'E' (Inventory)
		handleKey('Q', 20);         // 20 = 'Q' (Drop)
		handleKey('T', 23);         // 23 = 'T' (Chat)
		handleKey(VK_OEM_2, 56);    // 56 = '/' (Command)
		handleKey(VK_RETURN, 40);   // 40 = Return / Enter
		handleKey(VK_ESCAPE, 41);   // 41 = Escape
		handleKey(VK_SPACE, 44);    // 44 = Space
		handleKey(VK_SHIFT, 225);   // 225 = LShift (Sneak / Duck)
		handleKey(VK_CONTROL, 224); // 224 = LCtrl (Sprint)
		handleKey('W', 26);
		handleKey('A', 4);
		handleKey('S', 22);
		handleKey('D', 7);
		handleKey('F', 9);
		for (int i = 0; i < 9; i++) {
			handleKey('1' + i, 30 + i);
		}
		handleKey('0', 39);
	}

	void HL2Bridge::PushInput(uint16_t type, uint16_t code, int32_t a, int32_t b)
	{
		if (!m_shmView) return;
		uint64_t head = *(uint64_t*)(m_shmView + proto::kOffInputRing + proto::kInputRingHeadOff);
		uint64_t off = proto::kOffInputRing + proto::kInputRingDataOff + (head % proto::kInputRingEntries) * sizeof(proto::InputEvent);
		proto::InputEvent *ev = (proto::InputEvent*)(m_shmView + off);
		ev->type = type;
		ev->code = code;
		ev->a = a;
		ev->b = b;
		ev->c = 0;
		MemoryBarrier();
		*(uint64_t*)(m_shmView + proto::kOffInputRing + proto::kInputRingHeadOff) = head + 1;
	}

	void HL2Bridge::PushTextInput(int32_t codepoint)
	{
		PushInput(proto::kInText, 0, codepoint, 0);
	}

	LRESULT CALLBACK HL2Bridge::SubclassWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		HL2Bridge &b = HL2Bridge::Get();

		if (uMsg == WM_INPUT) {
			if (b.m_guiOpen) {
				return 0; // Prevent Source engine from locking or centering cursor while GUI is open!
			}
		}

		if (b.IsGuiOpen()) {
			if (uMsg == WM_SETCURSOR) {
				SetCursor(LoadCursor(NULL, IDC_ARROW));
				return TRUE;
			}
			if (uMsg == WM_MOUSEMOVE) {
				int x = (short)LOWORD(lParam);
				int y = (short)HIWORD(lParam);
				b.m_cursorX = std::clamp(x, 0, b.m_viewW - 1);
				b.m_cursorY = std::clamp(y, 0, b.m_viewH - 1);
				b.PushInput(proto::kInCursor, 0, b.m_cursorX, b.m_cursorY);
				SetCursor(LoadCursor(NULL, IDC_ARROW));
				return 0;
			}
			if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP) {
				b.PushInput(proto::kInMouseButton, 1, (uMsg == WM_LBUTTONDOWN) ? 1 : 0);
				return 0;
			}
			if (uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP) {
				b.PushInput(proto::kInMouseButton, 3, (uMsg == WM_RBUTTONDOWN) ? 1 : 0);
				return 0;
			}
			if (uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP) {
				b.PushInput(proto::kInMouseButton, 2, (uMsg == WM_MBUTTONDOWN) ? 1 : 0);
				return 0;
			}
			if (uMsg == WM_MOUSEWHEEL) {
				short delta = (short)HIWORD(wParam);
				b.PushInput(proto::kInScroll, 0, delta);
				return 0;
			}
			if (uMsg == WM_CHAR) {
				int32_t cp = (int32_t)wParam;
				if (cp >= 32 || cp == 8 || cp == 13) {
					b.PushTextInput(cp);
				}
				return 0;
			}
			if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP) {
				bool down = (uMsg == WM_KEYDOWN);
				int vk = (int)wParam;
				uint16_t scancode = 0;
				switch (vk) {
					case 'E':       scancode = 8; break;
					case VK_RETURN: scancode = 40; break;
					case VK_ESCAPE: scancode = 41; break;
					case VK_BACK:   scancode = 42; break;
					case VK_TAB:    scancode = 43; break;
					case VK_LEFT:   scancode = 80; break;
					case VK_RIGHT:  scancode = 79; break;
					case VK_UP:     scancode = 82; break;
					case VK_DOWN:   scancode = 81; break;
					case VK_DELETE: scancode = 76; break;
					case VK_HOME:   scancode = 74; break;
					case VK_END:    scancode = 77; break;
					default: break;
				}
				if (scancode != 0) {
					b.PushInput(proto::kInKey, scancode, down ? 1 : 0);
				}
				return 0;
			}
		}
		return CallWindowProcA(b.m_oldWndProc, hwnd, uMsg, wParam, lParam);
	}

	void HL2Bridge::HookWindow()
	{
		if (m_oldWndProc) return;
		HWND hwnd = FindWindowA("Valve001", nullptr);
		if (!hwnd) return;
		m_hValveWnd = hwnd;

		if (!g_realSetCursorPos) {
			HMODULE hUser32 = GetModuleHandleA("user32.dll");
			if (hUser32) {
				g_realSetCursorPos = (SetCursorPosFn)GetProcAddress(hUser32, "SetCursorPos");
				if (g_realSetCursorPos) {
					DWORD oldProt;
					if (VirtualProtect((void*)g_realSetCursorPos, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
						std::memcpy(g_savedCursorBytes, (void*)g_realSetCursorPos, 5);
						uint8_t jmp[5] = { 0xE9, 0, 0, 0, 0 };
						*(int32_t*)(jmp + 1) = (int32_t)((char*)Hook_SetCursorPos - (char*)g_realSetCursorPos - 5);
						std::memcpy((void*)g_realSetCursorPos, jmp, 5);
						VirtualProtect((void*)g_realSetCursorPos, 5, oldProt, &oldProt);
					}
				}
			}
		}

		RAWINPUTDEVICE rid;
		rid.usUsagePage = 0x01; // Generic Desktop Controls
		rid.usUsage = 0x02;     // Mouse
		rid.dwFlags = RIDEV_INPUTSINK;
		rid.hwndTarget = hwnd;
		RegisterRawInputDevices(&rid, 1, sizeof(rid));

		m_oldWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassWndProc);
	}

	void HL2Bridge::UnhookWindow()
	{
		if (g_realSetCursorPos && g_savedCursorBytes[0] != 0) {
			DWORD oldProt;
			if (VirtualProtect((void*)g_realSetCursorPos, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
				std::memcpy((void*)g_realSetCursorPos, g_savedCursorBytes, 5);
				VirtualProtect((void*)g_realSetCursorPos, 5, oldProt, &oldProt);
			}
			g_realSetCursorPos = nullptr;
		}
		if (m_oldWndProc && m_hValveWnd) {
			SetWindowLongPtrA(m_hValveWnd, GWLP_WNDPROC, (LONG_PTR)m_oldWndProc);
			m_oldWndProc = nullptr;
			m_hValveWnd = nullptr;
		}
	}
}
