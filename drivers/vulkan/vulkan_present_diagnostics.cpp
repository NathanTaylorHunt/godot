/**************************************************************************/
/*  vulkan_present_diagnostics.cpp                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "vulkan_present_diagnostics.h"

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/print_string.h"

namespace {

// The project setting that decides how long a GPU wait may run before it says so. Zero turns the
// reporting off and restores a single infinite wait.
constexpr const char *STALL_THRESHOLD_SETTING = "rendering/rendering_device/diagnostics/gpu_stall_report_threshold_msec";

// The directory an out-of-process watcher gives the engine to drop capture requests in. The game
// sets it when it starts the watcher, so a run with no watcher writes nothing.
constexpr const char *CAPTURE_DIRECTORY_VARIABLE = "GODOT_GPU_STALL_CAPTURE_DIR";

// One capture takes seconds and hundreds of megabytes, so a session that stalls over and over asks
// for a handful and then only writes to the log.
constexpr uint32_t MAX_CAPTURE_REQUESTS = 5;

// A wait that never comes back reports at its first four expiries and then once every twelfth, so
// a permanent stall stays visible in the log without filling it.
constexpr uint32_t ALWAYS_REPORTED_EXPIRIES = 4;
constexpr uint32_t PERIODIC_REPORT_EVERY = 12;

struct KnownModule {
	const char *file_name;
	const char *what;
};

// The modules the 2026-09-11 freeze analysis found sitting between the engine and the screen. A
// capture or overlay layer loads whether or not its application is running and wraps the same
// present and acquire calls a stalled frame is waiting on, so a report that does not say which of
// them were loaded cannot be compared with any other machine's.
constexpr KnownModule KNOWN_MODULES[] = {
	{ "graphics-hook64.dll", "OBS capture layer" },
	{ "RTSSVkLayer64.dll", "RivaTuner layer" },
	{ "RTSSHooks64.dll", "RivaTuner hook" },
	{ "nvspcap64.dll", "NVIDIA App overlay" },
	{ "gameoverlayrenderer64.dll", "Steam overlay" },
	{ "DiscordHook64.dll", "Discord overlay" },
	{ "EOSOVH-Win64-Shipping.dll", "Epic overlay" },
	{ "D3D12Core.dll", "DXGI present path" },
	{ "dxgi.dll", "DXGI present path" },
	{ "nvwgf2umx.dll", "DXGI present path" },
};

} //namespace

void VulkanPresentDiagnostics::configure() {
	GLOBAL_DEF_RST(PropertyInfo(Variant::INT, STALL_THRESHOLD_SETTING, PROPERTY_HINT_RANGE, "0,60000,100,suffix:ms"), 5000);
	const int threshold_msec = GLOBAL_GET(STALL_THRESHOLD_SETTING);
	stall_threshold = threshold_msec > 0 ? (uint64_t)threshold_msec * 1000 : 0;
}

const char *VulkanPresentDiagnostics::wait_name(WaitKind p_wait) {
	switch (p_wait) {
		case WAIT_SWAP_CHAIN_ACQUIRE:
			return "swap chain acquire";
		case WAIT_FENCE:
		default:
			return "frame fence wait";
	}
}

void VulkanPresentDiagnostics::log_presentation(const Presentation &p_presentation) {
	// A swap chain is recreated on every resize, but the mode it is granted changes only when the
	// application asks for a different one -- which it does, after boot, without saying so
	// anywhere. One line per configuration therefore says more than one line per launch, and still
	// costs one line on a run that never changes its mind.
	if (presentation_logged && present_mode_name == p_presentation.present_mode) {
		return;
	}

	const bool first = !presentation_logged;
	present_mode_name = p_presentation.present_mode;
	presentation_logged = true;

	String driver = p_presentation.driver_name;
	if (!p_presentation.driver_info.is_empty()) {
		driver += " " + p_presentation.driver_info;
	}
	if (driver.strip_edges().is_empty()) {
		driver = "unknown";
	}

	String layers = String(", ").join(p_presentation.loader_layers);
	if (layers.is_empty()) {
		layers = "none";
	}

	// The Vulkan version encoding, spelled out rather than pulled in with the Vulkan headers: this
	// file deliberately knows nothing about Vulkan beyond what the driver hands it.
	const uint32_t api_major = (p_presentation.api_version >> 22) & 0x7F;
	const uint32_t api_minor = (p_presentation.api_version >> 12) & 0x3FF;
	const uint32_t api_patch = p_presentation.api_version & 0xFFF;

	print_line(vformat(
			"%s: present mode %s (v-sync %s), %d swap chain images, %s; device %s, driver %s (raw %d), Vulkan %d.%d.%d; loader layers: %s; interceptors: %s.",
			String(first ? "Presentation" : "Presentation changed"),
			p_presentation.present_mode,
			p_presentation.vsync_mode,
			p_presentation.image_count,
			p_presentation.format,
			p_presentation.device_name,
			driver,
			p_presentation.driver_version,
			api_major,
			api_minor,
			api_patch,
			layers,
			describe_interceptors()));
}

String VulkanPresentDiagnostics::describe_interceptors() {
	const Vector<String> modules = OS::get_singleton()->get_loaded_module_names();
	if (modules.is_empty()) {
		return "not reported on this platform";
	}

	String described;
	for (const String &module : modules) {
		const char *what = nullptr;
		for (const KnownModule &known : KNOWN_MODULES) {
			if (module.nocasecmp_to(known.file_name) == 0) {
				what = known.what;
				break;
			}
		}
		if (what == nullptr && module.containsn("vklayer")) {
			// Any other implicit layer registered on the machine wraps the same calls.
			what = "implicit Vulkan layer";
		}
		if (what == nullptr) {
			continue;
		}
		if (!described.is_empty()) {
			described += ", ";
		}
		described += vformat("%s (%s)", module, String(what));
	}

	return described.is_empty() ? "none" : described;
}

void VulkanPresentDiagnostics::report_stall(WaitKind p_wait, uint64_t p_elapsed_usec, uint32_t p_report_index, const String &p_detail) {
	const String message = vformat(
			"GPU stall at %s: the %s has not returned after %.1f s. Present mode %s; %s. The wait continues; nothing has been cancelled.",
			Time::get_singleton()->get_datetime_string_from_system(false, true),
			String(wait_name(p_wait)),
			p_elapsed_usec / 1000000.0,
			present_mode_name,
			p_detail);

	if (p_report_index == 1) {
		write_capture_request(message);
	}

	if (p_report_index <= ALWAYS_REPORTED_EXPIRIES || (p_report_index % PERIODIC_REPORT_EVERY) == 0) {
		WARN_PRINT(message);
	}
}

void VulkanPresentDiagnostics::report_recovered(WaitKind p_wait, uint64_t p_elapsed_usec) {
	WARN_PRINT(vformat(
			"GPU stall ended: the %s returned after %.1f s.",
			String(wait_name(p_wait)),
			p_elapsed_usec / 1000000.0));
}

void VulkanPresentDiagnostics::request_forced_stall(uint32_t p_msec) {
	forced_stall_usec.set((uint64_t)p_msec * 1000);
}

uint64_t VulkanPresentDiagnostics::take_forced_stall_usec() {
	const uint64_t armed = forced_stall_usec.get();
	if (armed == 0) {
		return 0;
	}
	forced_stall_usec.set(0);
	return armed;
}

void VulkanPresentDiagnostics::write_capture_request(const String &p_message) {
	const String directory = OS::get_singleton()->get_environment(CAPTURE_DIRECTORY_VARIABLE);
	if (directory.is_empty() || capture_requests >= MAX_CAPTURE_REQUESTS) {
		return;
	}

	if (!DirAccess::dir_exists_absolute(directory) && DirAccess::make_dir_recursive_absolute(directory) != OK) {
		return;
	}

	capture_requests++;
	const String path = directory.path_join(vformat("stall-%d-%d.request", OS::get_singleton()->get_process_id(), capture_requests));
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		return;
	}
	file->store_string(p_message);
}
