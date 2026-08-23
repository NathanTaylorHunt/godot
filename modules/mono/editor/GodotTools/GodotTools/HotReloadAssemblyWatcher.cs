using Godot;
using GodotTools.Build;
using GodotTools.Internals;
using JetBrains.Annotations;

namespace GodotTools
{
    public partial class HotReloadAssemblyWatcher : Node
    {
#nullable disable
        private Timer _watchTimer;
#nullable enable

        public override void _Notification(int what)
        {
            if (what == Node.NotificationWMWindowFocusIn)
            {
                RestartTimer();
                TryReloadAssemblies();
            }
        }

        private void TimerTimeout()
        {
            TryReloadAssemblies();
        }

        private static void TryReloadAssemblies()
        {
            // A blocking build pumps the editor main loop to keep its progress dialog
            // responsive. Defer hot reload until the build returns so the watcher cannot
            // reload the project assembly re-entrantly from that nested main-loop pass.
            if (!BuildManager.IsBuildInProgress && Internal.IsAssembliesReloadingNeeded())
            {
                BuildManager.UpdateLastValidBuildDateTime();
                Internal.ReloadAssemblies(softReload: false);
            }
        }

        [UsedImplicitly]
        public void RestartTimer()
        {
            _watchTimer.Stop();
            _watchTimer.Start();
        }

        public override void _Ready()
        {
            base._Ready();

            _watchTimer = new Timer
            {
                OneShot = false,
                WaitTime = 0.5f
            };
            _watchTimer.Timeout += TimerTimeout;
            AddChild(_watchTimer);
            _watchTimer.Start();
        }
    }
}
