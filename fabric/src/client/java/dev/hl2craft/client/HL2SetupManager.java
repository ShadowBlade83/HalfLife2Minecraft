package dev.hl2craft.client;

import dev.hl2craft.HL2Craft;
import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import net.minecraft.client.Minecraft;

public final class HL2SetupManager {
	private static final Path CONFIG_PATH = Path.of("hl2craft_path.txt");
	private static Path cachedHL2Dir = null;

	public static Path findHL2Dir() {
		if (cachedHL2Dir != null && Files.exists(cachedHL2Dir.resolve("hl2.exe"))) {
			return cachedHL2Dir;
		}

		// 1. Saved path from config
		try {
			if (Files.exists(CONFIG_PATH)) {
				String line = Files.readString(CONFIG_PATH).trim();
				if (!line.isEmpty()) {
					Path p = Path.of(line);
					if (Files.exists(p.resolve("hl2.exe"))) {
						cachedHL2Dir = p;
						return p;
					}
				}
			}
		} catch (Exception ignored) {}

		// 2. System property
		String sysProp = System.getProperty("hl2.path");
		if (sysProp != null && !sysProp.isBlank()) {
			Path p = Path.of(sysProp.trim());
			if (Files.exists(p.resolve("hl2.exe"))) {
				setHL2Dir(p);
				return p;
			}
		}

		// 3. Scan Steam libraries
		List<Path> steamRoots = new ArrayList<>();

		// Query Windows registry for SteamPath
		try {
			Process proc = new ProcessBuilder("reg", "query", "HKCU\\Software\\Valve\\Steam", "/v", "SteamPath").start();
			try (BufferedReader reader = new BufferedReader(new InputStreamReader(proc.getInputStream()))) {
				String line;
				while ((line = reader.readLine()) != null) {
					if (line.contains("SteamPath") && line.contains("REG_SZ")) {
						String val = line.substring(line.indexOf("REG_SZ") + 6).trim();
						Path p = Path.of(val);
						if (Files.isDirectory(p)) steamRoots.add(p);
					}
				}
			}
		} catch (Exception ignored) {}

		// Standard drive locations
		for (char d = 'C'; d <= 'Z'; d++) {
			String drive = d + ":";
			steamRoots.add(Path.of(drive, "Program Files (x86)", "Steam"));
			steamRoots.add(Path.of(drive, "Program Files", "Steam"));
			steamRoots.add(Path.of(drive, "Steam"));
			steamRoots.add(Path.of(drive, "SteamLibrary"));
			steamRoots.add(Path.of(drive, "games", "Steam"));
			steamRoots.add(Path.of(drive, "Games", "Steam"));
		}

		Pattern vdfPathPattern = Pattern.compile("\"path\"\\s+\"([^\"]+)\"");
		for (Path root : steamRoots) {
			if (!Files.isDirectory(root)) continue;

			// Direct check
			Path directHl2 = root.resolve("steamapps/common/Half-Life 2");
			if (Files.exists(directHl2.resolve("hl2.exe"))) {
				setHL2Dir(directHl2);
				return directHl2;
			}

			// libraryfolders.vdf
			Path vdf = root.resolve("steamapps/libraryfolders.vdf");
			if (Files.exists(vdf)) {
				try {
					List<String> lines = Files.readAllLines(vdf);
					for (String line : lines) {
						Matcher m = vdfPathPattern.matcher(line);
						if (m.find()) {
							String libPathStr = m.group(1).replace("\\\\", "\\");
							Path libHl2 = Path.of(libPathStr, "steamapps", "common", "Half-Life 2");
							if (Files.exists(libHl2.resolve("hl2.exe"))) {
								setHL2Dir(libHl2);
								return libHl2;
							}
						}
					}
				} catch (Exception ignored) {}
			}
		}

		return null;
	}

	public static void setHL2Dir(Path dir) {
		cachedHL2Dir = dir;
		try {
			Files.writeString(CONFIG_PATH, dir.toAbsolutePath().toString(), StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
		} catch (Exception ignored) {}
	}

	public static boolean isReShadeInstalled(Path hl2Dir) {
		if (hl2Dir == null) return false;
		return Files.exists(hl2Dir.resolve("bin/d3d9.dll")) || Files.exists(hl2Dir.resolve("d3d9.dll"));
	}

	public static byte[] getEmbeddedAddonBytes() {
		try (InputStream in = HL2SetupManager.class.getResourceAsStream("/assets/hl2craft/HL2Minecraft.addon32")) {
			if (in != null) return in.readAllBytes();
		} catch (Exception ignored) {}
		return null;
	}

	public static byte[] getEmbeddedShaderBytes() {
		try (InputStream in = HL2SetupManager.class.getResourceAsStream("/assets/hl2craft/HL2Minecraft.fx")) {
			if (in != null) return in.readAllBytes();
		} catch (Exception ignored) {}
		return null;
	}

	public static boolean isAddonUpToDate(Path hl2Dir) {
		if (hl2Dir == null) return false;
		byte[] addon = getEmbeddedAddonBytes();
		if (addon == null) return true;

		Path target = hl2Dir.resolve("bin/HL2Minecraft.addon32");
		if (!Files.exists(target)) target = hl2Dir.resolve("HL2Minecraft.addon32");
		if (!Files.exists(target)) return false;

		try {
			return Files.size(target) == addon.length;
		} catch (Exception e) {
			return false;
		}
	}

	public static boolean installOrUpdateAddon(Path hl2Dir) {
		if (hl2Dir == null) return false;
		byte[] addon = getEmbeddedAddonBytes();
		if (addon == null) return false;

		try {
			Path target1 = hl2Dir.resolve("bin/HL2Minecraft.addon32");
			Path target2 = hl2Dir.resolve("HL2Minecraft.addon32");
			Files.createDirectories(target1.getParent());
			Files.write(target1, addon, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
			Files.write(target2, addon, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
			HL2Craft.LOG.info("HL2Craft: Installed/Updated HL2Minecraft.addon32 in {}", hl2Dir);

			byte[] shader = getEmbeddedShaderBytes();
			if (shader != null) {
				Path shaderTarget = hl2Dir.resolve("bin/reshade-shaders/Shaders/HL2Minecraft.fx");
				Files.createDirectories(shaderTarget.getParent());
				Files.write(shaderTarget, shader, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
			}

			// Ensure ReShade.ini does not disable addon
			Path iniPath = hl2Dir.resolve("bin/ReShade.ini");
			if (!Files.exists(iniPath)) iniPath = hl2Dir.resolve("ReShade.ini");
			if (Files.exists(iniPath)) {
				String iniContent = Files.readString(iniPath);
				if (iniContent.contains("DisabledAddons=HL2Minecraft")) {
					iniContent = iniContent.replace("DisabledAddons=HL2Minecraft", "DisabledAddons=");
					Files.writeString(iniPath, iniContent);
				}
			}

			// Ensure custom autoexec.cfg exists
			try {
				Path cfgDir = hl2Dir.resolve("hl2/custom/hl2_minecraft/cfg");
				Files.createDirectories(cfgDir);
				Path autoexec = cfgDir.resolve("autoexec.cfg");
				if (!Files.exists(autoexec)) {
					String autoexecContent = "sv_cheats 1\ncon_enable 1\nr_drawviewmodel 0\ncrosshair 0\n";
					Files.writeString(autoexec, autoexecContent, StandardOpenOption.CREATE);
				}
			} catch (Exception ignored) {}

			return true;
		} catch (Exception e) {
			HL2Craft.LOG.error("HL2Craft: Failed to install addon: {}", e.getMessage());
			return false;
		}
	}

	public static boolean isHL2Running() {
		try {
			return ProcessHandle.allProcesses().anyMatch(p -> {
				String cmd = p.info().command().orElse("").toLowerCase();
				return cmd.endsWith("hl2.exe");
			});
		} catch (Exception e) {
			return false;
		}
	}

	public static void launchHL2(Path hl2Dir) {
		if (isHL2Running()) return;

		try {
			HL2Craft.LOG.info("HL2Craft: Launching Half-Life 2 via Steam protocol...");
			new ProcessBuilder("cmd", "/c", "start", "steam://rungameid/220").start();
		} catch (Exception e) {
			HL2Craft.LOG.warn("Steam URL launch failed, launching hl2.exe directly...");
			if (hl2Dir != null) {
				Path exe = hl2Dir.resolve("hl2.exe");
				if (Files.exists(exe)) {
					try {
						new ProcessBuilder(exe.toString(), "-game", "hl2", "-windowed", "-noborder")
							.directory(hl2Dir.toFile())
							.start();
					} catch (Exception ex) {
						HL2Craft.LOG.error("Direct hl2.exe launch failed: {}", ex.getMessage());
					}
				}
			}
		}
	}

	public static void openUrl(String url) {
		try {
			new ProcessBuilder("cmd", "/c", "start", url).start();
		} catch (Exception ignored) {}
	}

	public static Path browseForHL2Folder() {
		try {
			String script = "Add-Type -AssemblyName System.Windows.Forms; $f = New-Object System.Windows.Forms.FolderBrowserDialog; $f.Description = 'Выберите папку Half-Life 2'; if ($f.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { Write-Output $f.SelectedPath }";
			Process proc = new ProcessBuilder("powershell", "-NoProfile", "-Command", script).start();
			try (BufferedReader reader = new BufferedReader(new InputStreamReader(proc.getInputStream()))) {
				String line = reader.readLine();
				if (line != null && !line.isBlank()) {
					Path selected = Path.of(line.trim());
					if (Files.exists(selected.resolve("hl2.exe"))) {
						setHL2Dir(selected);
						return selected;
					}
				}
			}
		} catch (Exception ignored) {}
		return null;
	}

	public static void checkAndProceed(Minecraft minecraft, Runnable onComplete) {
		Path hl2Dir = findHL2Dir();
		boolean reshade = isReShadeInstalled(hl2Dir);

		if (hl2Dir == null || !reshade) {
			minecraft.gui.setScreen(new HL2SetupScreen(minecraft, onComplete));
		} else {
			installOrUpdateAddon(hl2Dir);
			if (!isHL2Running()) {
				launchHL2(hl2Dir);
			}
			onComplete.run();
		}
	}
}
