using System.Drawing;
using System.Threading.Tasks;
using System.Diagnostics;
using System;
using System.IO;
using NoFences.Win32;
using NoFences.Util;

namespace NoFences.Model
{
    public class FenceEntry
    {
        public string Path { get; }

        public EntryType Type { get; }

        public string Name => System.IO.Path.GetFileNameWithoutExtension(Path);

        private FenceEntry(string path, EntryType type)
        {
            Path = path;
            Type = type;
        }

        public static FenceEntry FromPath(string path)
        {
            if (File.Exists(path))
                return new FenceEntry(path, EntryType.File);
            else if (Directory.Exists(path))
                return new FenceEntry(path, EntryType.Folder);
            else return null;
        }

        public Icon ExtractIcon(ThumbnailProvider thumbnailProvider)
        {
            if (Type == EntryType.File)
            {
                if (thumbnailProvider.IsSupported(Path))
                    return thumbnailProvider.GenerateThumbnail(Path);
                else
                    return Icon.ExtractAssociatedIcon(Path);
            }
            else
            {
                return IconUtil.FolderLarge;
            }
        }

        public void Open()
        {
            Task.Run(() =>
            {
                // start asynchronously
                try
                {
					ProcessStartInfo psi = new ProcessStartInfo();
					psi.FileName = Path;
					psi.WorkingDirectory = System.IO.Path.GetDirectoryName(Path);
					if (Type == EntryType.Folder)
                    {
                        psi.WorkingDirectory = Path;
                        psi.Arguments = Path;
						psi.FileName = "explorer.exe";
					}
					Process.Start(psi);
                }
                catch (Exception e)
                {
                    Console.WriteLine($"Failed to start: {e}");
                }
            });
        }
    }
}
