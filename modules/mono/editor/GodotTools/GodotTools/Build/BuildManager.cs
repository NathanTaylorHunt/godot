using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Threading.Tasks;
using Godot;
using GodotTools.Internals;
using File = GodotTools.Utils.File;

namespace GodotTools.Build
{
    public static class BuildManager
    {
        private static BuildInfo? _buildInProgress;

        public static bool IsBuildInProgress => _buildInProgress != null;

        public const string MsBuildIssuesFileName = "msbuild_issues.csv";
        private const string MsBuildLogFileName = "msbuild_log.txt";

        public delegate void BuildLaunchFailedEventHandler(BuildInfo buildInfo, string reason);

        public static event BuildLaunchFailedEventHandler? BuildLaunchFailed;
        public static event Action<BuildInfo>? BuildStarted;
        public static event Action<BuildResult>? BuildFinished;
        public static event Action<string?>? StdOutputReceived;
        public static event Action<string?>? StdErrorReceived;

        public static DateTime LastValidBuildDateTime { get; private set; }

        static BuildManager()
        {
            UpdateLastValidBuildDateTime();
        }

        public static void UpdateLastValidBuildDateTime()
        {
            var dllName = $"{GodotSharpDirs.ProjectAssemblyName}.dll";
            var path = Path.Combine(GodotSharpDirs.ProjectBaseOutputPath, "Debug", dllName);
            LastValidBuildDateTime = File.GetLastWriteTime(path);
        }

        private static void RemoveOldIssuesFile(BuildInfo buildInfo)
        {
            string issuesFile = GetIssuesFilePath(buildInfo);

            if (!File.Exists(issuesFile))
                return;

            File.Delete(issuesFile);
        }

        private static void ShowBuildErrorDialog(string message)
        {
            var plugin = GodotSharpEditor.Instance;
            plugin.ShowErrorDialog(message, "Build error");
            plugin.MSBuildPanel.MakeVisible();
        }

        private static string GetLogFilePath(BuildInfo buildInfo)
        {
            return Path.Combine(buildInfo.LogsDirPath, MsBuildLogFileName);
        }

        private static string GetIssuesFilePath(BuildInfo buildInfo)
        {
            return Path.Combine(buildInfo.LogsDirPath, MsBuildIssuesFileName);
        }

        private static void PrintVerbose(string text)
        {
            if (OS.IsStdOutVerbose())
                GD.Print(text);
        }

        private static bool Build(BuildInfo buildInfo, Func<bool>? pumpProgress = null)
        {
            if (_buildInProgress != null)
                throw new InvalidOperationException("A build is already in progress.");

            _buildInProgress = buildInfo;

            try
            {
                BuildStarted?.Invoke(buildInfo);

                // Required in order to update the build tasks list.
                Internal.GodotMainIteration();

                try
                {
                    RemoveOldIssuesFile(buildInfo);
                }
                catch (IOException e)
                {
                    BuildLaunchFailed?.Invoke(buildInfo, $"Cannot remove issues file: {GetIssuesFilePath(buildInfo)}");
                    Console.Error.WriteLine(e);
                }

                try
                {
                    int exitCode = BuildSystem.Build(buildInfo, StdOutputReceived, StdErrorReceived, pumpProgress);

                    if (exitCode != 0)
                        PrintVerbose($"MSBuild exited with code: {exitCode}. Log file: {GetLogFilePath(buildInfo)}");

                    BuildFinished?.Invoke(exitCode == 0 ? BuildResult.Success : BuildResult.Error);

                    return exitCode == 0;
                }
                catch (Exception e)
                {
                    BuildLaunchFailed?.Invoke(buildInfo,
                        $"The build method threw an exception.\n{e.GetType().FullName}: {e.Message}");
                    Console.Error.WriteLine(e);
                    return false;
                }
            }
            finally
            {
                _buildInProgress = null;
            }
        }

        public static async Task<bool> BuildAsync(BuildInfo buildInfo)
        {
            if (_buildInProgress != null)
                throw new InvalidOperationException("A build is already in progress.");

            _buildInProgress = buildInfo;

            try
            {
                BuildStarted?.Invoke(buildInfo);

                try
                {
                    RemoveOldIssuesFile(buildInfo);
                }
                catch (IOException e)
                {
                    BuildLaunchFailed?.Invoke(buildInfo, $"Cannot remove issues file: {GetIssuesFilePath(buildInfo)}");
                    Console.Error.WriteLine(e);
                }

                try
                {
                    int exitCode = await BuildSystem.BuildAsync(buildInfo, StdOutputReceived, StdErrorReceived);

                    if (exitCode != 0)
                        PrintVerbose($"MSBuild exited with code: {exitCode}. Log file: {GetLogFilePath(buildInfo)}");

                    BuildFinished?.Invoke(exitCode == 0 ? BuildResult.Success : BuildResult.Error);

                    return exitCode == 0;
                }
                catch (Exception e)
                {
                    BuildLaunchFailed?.Invoke(buildInfo,
                        $"The build method threw an exception.\n{e.GetType().FullName}: {e.Message}");
                    Console.Error.WriteLine(e);
                    return false;
                }
            }
            finally
            {
                _buildInProgress = null;
            }
        }

        private static bool Publish(BuildInfo buildInfo, Func<bool>? pumpProgress = null)
        {
            if (_buildInProgress != null)
                throw new InvalidOperationException("A build is already in progress.");

            _buildInProgress = buildInfo;

            try
            {
                BuildStarted?.Invoke(buildInfo);

                // Required in order to update the build tasks list.
                Internal.GodotMainIteration();

                try
                {
                    RemoveOldIssuesFile(buildInfo);
                }
                catch (IOException e)
                {
                    BuildLaunchFailed?.Invoke(buildInfo, $"Cannot remove issues file: {GetIssuesFilePath(buildInfo)}");
                    Console.Error.WriteLine(e);
                }

                try
                {
                    int exitCode = BuildSystem.Publish(buildInfo, StdOutputReceived, StdErrorReceived, pumpProgress);

                    if (exitCode != 0)
                        PrintVerbose(
                            $"dotnet publish exited with code: {exitCode}. Log file: {GetLogFilePath(buildInfo)}");

                    BuildFinished?.Invoke(exitCode == 0 ? BuildResult.Success : BuildResult.Error);

                    return exitCode == 0;
                }
                catch (Exception e)
                {
                    BuildLaunchFailed?.Invoke(buildInfo,
                        $"The publish method threw an exception.\n{e.GetType().FullName}: {e.Message}");
                    Console.Error.WriteLine(e);
                    return false;
                }
            }
            finally
            {
                _buildInProgress = null;
            }
        }

        private static bool RunWithProgress(BuildInfo buildInfo, Func<BuildInfo, Func<bool>?, bool> action,
            string task, string title, string stateLabel, out bool canceled)
        {
            bool success;
            bool cancelRequested = false;
            using (var pr = new EditorProgress(task, title, 1, canCancel: true))
            {
                var elapsed = Stopwatch.StartNew();
                pr.Step(stateLabel, 0);
                // The progress pump keeps the editor main loop iterating while the dotnet
                // process runs, so the window stays responsive, build output streams into
                // the MSBuild panel, and the Cancel button works.
                success = action(buildInfo, () =>
                {
                    if (pr.TryStep($"{stateLabel} ({elapsed.Elapsed.TotalSeconds:0} s)", 0, forceRefresh: false))
                        cancelRequested = true;
                    return cancelRequested;
                });
            }

            canceled = cancelRequested;
            return success;
        }

        private static bool BuildProjectBlocking(BuildInfo buildInfo)
        {
            if (!File.Exists(buildInfo.Project))
                return true; // No project to build.

            bool success = RunWithProgress(buildInfo, static (info, pump) => Build(info, pump),
                "dotnet_build_project", "Building .NET project...", "Building project", out bool canceled);

            if (!success && !canceled)
            {
                ShowBuildErrorDialog("Failed to build project. Check MSBuild panel for details.");
            }

            return success;
        }

        private static bool CleanProjectBlocking(BuildInfo buildInfo)
        {
            if (!File.Exists(buildInfo.Project))
                return true; // No project to clean.

            bool success = RunWithProgress(buildInfo, static (info, pump) => Build(info, pump),
                "dotnet_clean_project", "Cleaning .NET project...", "Cleaning project", out bool canceled);

            if (!success && !canceled)
            {
                ShowBuildErrorDialog("Failed to clean project");
            }

            return success;
        }

        private static bool PublishProjectBlocking(BuildInfo buildInfo)
        {
            return RunWithProgress(buildInfo, static (info, pump) => Publish(info, pump),
                "dotnet_publish_project", "Publishing .NET project...", "Running dotnet publish", out _);
        }

        private static BuildInfo CreateBuildInfo(
            string configuration,
            string? platform = null,
            bool rebuild = false,
            bool onlyClean = false
        )
        {
            var buildInfo = new BuildInfo(GodotSharpDirs.ProjectSlnPath, GodotSharpDirs.ProjectCsProjPath, configuration,
                restore: true, rebuild, onlyClean);

            // If a platform was not specified, try determining the current one. If that fails, let MSBuild auto-detect it.
            if (platform != null || Utils.OS.PlatformNameMap.TryGetValue(OS.GetName(), out platform))
                buildInfo.CustomProperties.Add($"GodotTargetPlatform={platform}");

            if (Internal.GodotIsRealTDouble())
                buildInfo.CustomProperties.Add("GodotFloat64=true");

            return buildInfo;
        }

        private static BuildInfo CreatePublishBuildInfo(
            string configuration,
            string platform,
            string runtimeIdentifier,
            string publishOutputDir,
            bool includeDebugSymbols = true
        )
        {
            var buildInfo = new BuildInfo(GodotSharpDirs.ProjectSlnPath, GodotSharpDirs.ProjectCsProjPath, configuration,
                runtimeIdentifier, publishOutputDir, restore: true, rebuild: false, onlyClean: false);

            if (!includeDebugSymbols)
            {
                buildInfo.CustomProperties.Add("DebugType=None");
                buildInfo.CustomProperties.Add("DebugSymbols=false");
            }

            buildInfo.CustomProperties.Add($"GodotTargetPlatform={platform}");

            if (Internal.GodotIsRealTDouble())
                buildInfo.CustomProperties.Add("GodotFloat64=true");

            return buildInfo;
        }

        public static bool BuildProjectBlocking(
            string configuration,
            string? platform = null,
            bool rebuild = false
        ) => BuildProjectBlocking(CreateBuildInfo(configuration, platform, rebuild));

        public static bool CleanProjectBlocking(
            string configuration,
            string? platform = null
        ) => CleanProjectBlocking(CreateBuildInfo(configuration, platform, rebuild: false, onlyClean: true));

        public static bool PublishProjectBlocking(
            string configuration,
            string platform,
            string runtimeIdentifier,
            string publishOutputDir,
            bool includeDebugSymbols = true
        ) => PublishProjectBlocking(CreatePublishBuildInfo(configuration,
            platform, runtimeIdentifier, publishOutputDir, includeDebugSymbols));

        public static bool GenerateXCFrameworkBlocking(
            List<string> outputPaths,
            string xcFrameworkPath)
        {
            using var pr = new EditorProgress("generate_xcframework", "Generating XCFramework...", 1);

            pr.Step("Running xcodebuild -create-xcframework", 0);

            if (!GenerateXCFramework(outputPaths, xcFrameworkPath))
            {
                ShowBuildErrorDialog("Failed to generate XCFramework");
                return false;
            }

            return true;
        }

        private static bool GenerateXCFramework(List<string> outputPaths, string xcFrameworkPath)
        {
            // Required in order to update the build tasks list.
            Internal.GodotMainIteration();

            try
            {
                int exitCode = BuildSystem.GenerateXCFramework(outputPaths, xcFrameworkPath, StdOutputReceived, StdErrorReceived);

                if (exitCode != 0)
                    PrintVerbose(
                        $"xcodebuild create-xcframework exited with code: {exitCode}.");

                return exitCode == 0;
            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e);
                return false;
            }
        }

        public static bool EditorBuildCallback()
        {
            if (!File.Exists(GodotSharpDirs.ProjectCsProjPath))
                return true; // No project to build.

            if (GodotSharpEditor.Instance.SkipBuildBeforePlaying)
                return true; // Requested play from an external editor/IDE which already built the project.

            return BuildProjectBlocking("Debug");
        }

        public static void Initialize()
        {
        }
    }
}
