using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Diagnostics;

// Generate grid "ruler" pattern image.
// Set as desktop wallpaper for easy window coordinate readout.

namespace test_pattern
{
    class Program
    {
        static int Main(string[] args)
        {
            int width, height;

            if (args.Length < 2)
            {
                Console.WriteLine("Usage: {0} <width> <height>", Process.GetCurrentProcess().ProcessName);
                return 1;
            }
            
            width = int.Parse(args[0]);
            height = int.Parse(args[1]);

            Bitmap bmp = new Bitmap(width, height, PixelFormat.Format24bppRgb);

            // grid
            for (int x = 0; x<width; x++)
            {
                for (int y=0; y<height; y++)
                {
                    if ((x % 10) * (y % 10) == 0)
                        bmp.SetPixel(x, y, Color.FromArgb(0x004040));

                    if ((x % 50) * (y % 50) == 0)
                        bmp.SetPixel(x, y, Color.FromArgb(0x206060));

                    if ((x % 100) * (y % 100) == 0)
                        bmp.SetPixel(x, y, Color.FromArgb(0xc04000));
                }
            }

            // numbers
            using (Graphics g = Graphics.FromImage(bmp))
            {
                Font font = new Font("Arial", 8);
                SolidBrush brush = new SolidBrush(Color.White);
                StringFormat sf = new StringFormat();

                for (int x = 0; x < width; x += 100)
                    g.DrawString(x.ToString(), font, brush, x + 2, 0);

                for (int y = 100; y < height; y += 100)
                    g.DrawString(y.ToString(), font, brush, 0, y + 2);
            }

            bmp.Save(String.Format("{0}x{1}.png", width, height), ImageFormat.Png);

            return 0;
        }
    }
}
