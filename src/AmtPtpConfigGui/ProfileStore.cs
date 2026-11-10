using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AmtPtpConfigGui
{
    internal sealed class GuiProfile
    {
        public string Name { get; set; } = "Profile 1";
        public Native.PalmConfig Palm { get; set; }
        public Native.PointerConfig Pointer { get; set; }
        public Native.ScrollConfig Scroll { get; set; }

        // LEGACY, read-only: the separate haptic settings of builds in which
        // this app played the second click itself. Null for every profile
        // saved before or after that period - deliberately NO initializer, so
        // "absent" stays distinguishable from "present". Consumed once by
        // ProfileStore.Normalize (PointerConfig.Migrate) and then dropped, so
        // it is never written back.
        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public Native.HapticsConfig? Haptics { get; set; }

        public override string ToString() => Name;
    }

    internal static class ProfileStore
    {
        private const int DefaultProfileCount = 3;
        private const string FileName = "profiles.json";
        private static readonly string DirectoryPath = SettingsPaths.DirectoryPath;
        private static readonly string FilePath = Path.Combine(DirectoryPath, FileName);
        private static readonly string TempFilePath = FilePath + ".tmp";
        private static readonly string BackupFilePath = FilePath + ".bak";
        private static readonly string CorruptBackupFilePath = FilePath + ".corrupt";
        private static readonly JsonSerializerOptions JsonOptions = new JsonSerializerOptions
        {
            WriteIndented = true,
            IncludeFields = true
        };

        public static List<GuiProfile> Load()
        {
            SettingsPaths.MigrateLegacyFile(FileName);

            foreach (string candidate in new[] { FilePath, BackupFilePath })
            {
                try
                {
                    if (!File.Exists(candidate))
                        continue;

                    var json = File.ReadAllText(candidate);
                    var profiles = JsonSerializer.Deserialize<List<GuiProfile>>(json, JsonOptions);
                    if (profiles != null && profiles.Count > 0)
                    {
                        Normalize(profiles);
                        return profiles;
                    }
                }
                catch
                {
                    // Try the backup before falling back to defaults. Never
                    // overwrite a corrupt main file merely by starting the GUI.
                }
            }

            PreserveCorruptMainFile();

            var defaults = new List<GuiProfile>();
            for (int i = 1; i <= DefaultProfileCount; i++)
                defaults.Add(CreateDefault($"Profile {i}"));

            try
            {
                Save(defaults);
            }
            catch
            {
                // The GUI can still start with in-memory defaults even when
                // the settings directory is not writable.
            }

            return defaults;
        }

        public static void Save(List<GuiProfile> profiles)
        {
            Directory.CreateDirectory(DirectoryPath);
            Normalize(profiles);
            var json = JsonSerializer.Serialize(profiles, JsonOptions);

            WriteAtomic(FilePath, TempFilePath, BackupFilePath, json);
        }

        private static void WriteAtomic(string destination, string temporary, string backup, string contents)
        {
            try
            {
                using (var stream = new FileStream(
                    temporary, FileMode.Create, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
                using (var writer = new StreamWriter(stream))
                {
                    writer.Write(contents);
                    writer.Flush();
                    stream.Flush(true);
                }

                if (File.Exists(destination))
                    File.Replace(temporary, destination, backup, true);
                else
                    File.Move(temporary, destination);
            }
            finally
            {
                // A failed replacement must not leave a stale .tmp file that
                // can be mistaken for a valid profile database later.
                try
                {
                    if (File.Exists(temporary))
                        File.Delete(temporary);
                }
                catch
                {
                }
            }
        }

        private static void PreserveCorruptMainFile()
        {
            try
            {
                if (File.Exists(FilePath))
                    File.Copy(FilePath, CorruptBackupFilePath, true);
            }
            catch
            {
            }
        }

        public static GuiProfile CreateDefault(string name)
        {
            return new GuiProfile
            {
                Name = name,
                Palm = Native.PalmConfig.Default,
                Pointer = Native.PointerConfig.Default,
                Scroll = Native.ScrollConfig.Default,
            };
        }

        // Internal (not private) so backup restore runs loaded profiles
        // through exactly the same normalization as the profile store.
        internal static void Normalize(List<GuiProfile> profiles)
        {
            foreach (var p in profiles)
            {
                if (string.IsNullOrWhiteSpace(p.Name))
                    p.Name = "Profile";
                else
                    p.Name = p.Name.Trim();

                p.Palm = p.Palm.StructVersion == 0 ? Native.PalmConfig.Default : p.Palm.Clamped();

                // All of the pointer version history lives in
                // PointerConfig.Migrate. The legacy haptic settings are folded
                // into the pointer config there and then dropped: the pointer
                // config is the only place they are kept from now on.
                p.Pointer = Native.PointerConfig.Migrate(p.Pointer, p.Haptics);
                p.Haptics = null;

                if (p.Scroll.StructVersion < Native.ScrollConfig.CurrentVersion)
                {
                    var defaults = Native.ScrollConfig.Default;
                    var old = p.Scroll;
                    old.ScaleNum = defaults.ScaleNum;
                    old.ScaleDen = defaults.ScaleDen;
                    old.ScaleNumFast = defaults.ScaleNumFast;
                    old.ScaleDenFast = defaults.ScaleDenFast;
                    p.Scroll = old;
                }
                p.Scroll = p.Scroll.StructVersion == 0 ? Native.ScrollConfig.Default : p.Scroll.Clamped();
            }
        }
    }
}
