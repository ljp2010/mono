//
// JpegCodec class testing unit
//
// Authors:
// 	Jordi Mas i Hernàndez (jordi@ximian.com)
//	Sebastien Pouliot  <sebastien@ximian.com>
//
// (C) 2004 Ximian, Inc.  http://www.ximian.com
// Copyright (C) 2004-2006 Novell, Inc (http://www.novell.com)
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

using System;
using System.Drawing;
using System.Drawing.Imaging;
using NUnit.Framework;
using System.IO;
using System.Security.Permissions;

namespace MonoTests.System.Drawing.Imaging {

	[TestFixture]
	[SecurityPermission (SecurityAction.Deny, UnmanagedCode = true)]
	public class JpegCodecTest {

		/* Get suffix to add to the filename */
		internal string getOutSufix ()
		{
			string s;

			int p = (int) Environment.OSVersion.Platform;
			if ((p == 4) || (p == 128))
				s = "-unix";
			else
				s = "-windows";

			if (Type.GetType ("Mono.Runtime", false) == null)
				s += "-msnet";
			else
				s += "-mono";

			return s;
		}

		/* Get the input directory depending on the runtime*/
		internal string getInFile (string file)
		{				
			string sRslt = Path.GetFullPath ("../System.Drawing/" + file);
				
			if (!File.Exists (sRslt)) 
				sRslt = "Test/System.Drawing/" + file;							
			
			return sRslt;
		}
		
		/* Checks bitmap features on a know 24-bits bitmap */
		[Test]
		public void Bitmap24bitFeatures ()
		{
			string sInFile = getInFile ("bitmaps/nature24bits.jpg");
			using (Bitmap bmp = new Bitmap (sInFile)) {
				GraphicsUnit unit = GraphicsUnit.World;
				RectangleF rect = bmp.GetBounds (ref unit);

				Assert.AreEqual (PixelFormat.Format24bppRgb, bmp.PixelFormat, "PixelFormat");
				Assert.AreEqual (110, bmp.Width, "bmp.Width");
				Assert.AreEqual (100, bmp.Height, "bmp.Height");

				Assert.AreEqual (0, rect.X, "rect.X");
				Assert.AreEqual (0, rect.Y, "rect.Y");
				Assert.AreEqual (110, rect.Width, "rect.Width");
				Assert.AreEqual (100, rect.Height, "rect.Height");

				Assert.AreEqual (110, bmp.Size.Width, "bmp.Size.Width");
				Assert.AreEqual (100, bmp.Size.Height, "bmp.Size.Height");
			}
		}

		[Test]
		public void Bitmap24bitPixels ()
		{
			string sInFile = getInFile ("bitmaps/nature24bits.jpg");
			using (Bitmap bmp = new Bitmap (sInFile)) {
#if false
				for (int x = 0; x < bmp.Width; x += 32) {
					for (int y = 0; y < bmp.Height; y += 32)
						Console.WriteLine ("\t\t\t\tAssert.AreEqual ({0}, bmp.GetPixel ({1}, {2}).ToArgb (), \"{1},{2}\");", bmp.GetPixel (x, y).ToArgb (), x, y);
				}
#else
				// sampling values from a well known bitmap
				Assert.AreEqual (-10447423, bmp.GetPixel (0, 0).ToArgb (), "0,0");
				Assert.AreEqual (-12171958, bmp.GetPixel (0, 32).ToArgb (), "0,32");
				Assert.AreEqual (-15192259, bmp.GetPixel (0, 64).ToArgb (), "0,64");
				Assert.AreEqual (-15131110, bmp.GetPixel (0, 96).ToArgb (), "0,96");
				Assert.AreEqual (-395272, bmp.GetPixel (32, 0).ToArgb (), "32,0");
				Assert.AreEqual (-10131359, bmp.GetPixel (32, 32).ToArgb (), "32,32");
				Assert.AreEqual (-10984322, bmp.GetPixel (32, 64).ToArgb (), "32,64");
				Assert.AreEqual (-11034683, bmp.GetPixel (32, 96).ToArgb (), "32,96");
				Assert.AreEqual (-1, bmp.GetPixel (64, 0).ToArgb (), "64,0");
				Assert.AreEqual (-3163242, bmp.GetPixel (64, 32).ToArgb (), "64,32");
				Assert.AreEqual (-7311538, bmp.GetPixel (64, 64).ToArgb (), "64,64");
				Assert.AreEqual (-12149780, bmp.GetPixel (64, 96).ToArgb (), "64,96");
				Assert.AreEqual (-1, bmp.GetPixel (96, 0).ToArgb (), "96,0");
				Assert.AreEqual (-8224378, bmp.GetPixel (96, 32).ToArgb (), "96,32");
				Assert.AreEqual (-11053718, bmp.GetPixel (96, 64).ToArgb (), "96,64");
				Assert.AreEqual (-12944166, bmp.GetPixel (96, 96).ToArgb (), "96,96");
#endif
			}
		}

		[Test]
		[Category ("NotWorking")]
		public void Bitmap24bitData ()
		{
			string sInFile = getInFile ("bitmaps/almogaver24bits.bmp");
			using (Bitmap bmp = new Bitmap (sInFile)) {
				BitmapData data = bmp.LockBits (new Rectangle (0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
				try {
					Assert.AreEqual (bmp.Height, data.Height, "Height");
					Assert.AreEqual (bmp.Width, data.Width, "Width");
					Assert.AreEqual (PixelFormat.Format24bppRgb, data.PixelFormat, "PixelFormat");
					int size = data.Height * data.Stride;
					unsafe {
						byte* scan = (byte*) data.Scan0;
#if false
						// 1009 is the first prime after 1000 (so we're not affected by a recurring pattern)
						for (int p = 0; p < size; p += 1009) {
							Console.WriteLine ("\t\t\t\t\t\tAssert.AreEqual ({0}, *(scan + {1}), \"{1}\");", *(scan + p), p);
						}
#else
						// sampling values from a well known bitmap
						Assert.AreEqual (217, *(scan + 0), "0");
						Assert.AreEqual (192, *(scan + 1009), "1009");
						Assert.AreEqual (210, *(scan + 2018), "2018");
						Assert.AreEqual (196, *(scan + 3027), "3027");
						Assert.AreEqual (216, *(scan + 4036), "4036");
						Assert.AreEqual (215, *(scan + 5045), "5045");
						Assert.AreEqual (218, *(scan + 6054), "6054");
						Assert.AreEqual (218, *(scan + 7063), "7063");
						Assert.AreEqual (95, *(scan + 8072), "8072");
						Assert.AreEqual (9, *(scan + 9081), "9081");
						Assert.AreEqual (247, *(scan + 10090), "10090");
						Assert.AreEqual (161, *(scan + 11099), "11099");
						Assert.AreEqual (130, *(scan + 12108), "12108");
						Assert.AreEqual (131, *(scan + 13117), "13117");
						Assert.AreEqual (175, *(scan + 14126), "14126");
						Assert.AreEqual (217, *(scan + 15135), "15135");
						Assert.AreEqual (201, *(scan + 16144), "16144");
						Assert.AreEqual (183, *(scan + 17153), "17153");
						Assert.AreEqual (236, *(scan + 18162), "18162");
						Assert.AreEqual (242, *(scan + 19171), "19171");
						Assert.AreEqual (125, *(scan + 20180), "20180");
						Assert.AreEqual (193, *(scan + 21189), "21189");
						Assert.AreEqual (227, *(scan + 22198), "22198");
						Assert.AreEqual (44, *(scan + 23207), "23207");
						Assert.AreEqual (230, *(scan + 24216), "24216");
						Assert.AreEqual (224, *(scan + 25225), "25225");
						Assert.AreEqual (164, *(scan + 26234), "26234");
						Assert.AreEqual (43, *(scan + 27243), "27243");
						Assert.AreEqual (200, *(scan + 28252), "28252");
						Assert.AreEqual (255, *(scan + 29261), "29261");
						Assert.AreEqual (226, *(scan + 30270), "30270");
						Assert.AreEqual (230, *(scan + 31279), "31279");
						Assert.AreEqual (178, *(scan + 32288), "32288");
						Assert.AreEqual (224, *(scan + 33297), "33297");
						Assert.AreEqual (233, *(scan + 34306), "34306");
						Assert.AreEqual (212, *(scan + 35315), "35315");
						Assert.AreEqual (153, *(scan + 36324), "36324");
						Assert.AreEqual (143, *(scan + 37333), "37333");
						Assert.AreEqual (215, *(scan + 38342), "38342");
						Assert.AreEqual (116, *(scan + 39351), "39351");
						Assert.AreEqual (26, *(scan + 40360), "40360");
						Assert.AreEqual (28, *(scan + 41369), "41369");
						Assert.AreEqual (75, *(scan + 42378), "42378");
						Assert.AreEqual (50, *(scan + 43387), "43387");
						Assert.AreEqual (244, *(scan + 44396), "44396");
						Assert.AreEqual (191, *(scan + 45405), "45405");
						Assert.AreEqual (200, *(scan + 46414), "46414");
						Assert.AreEqual (197, *(scan + 47423), "47423");
						Assert.AreEqual (232, *(scan + 48432), "48432");
						Assert.AreEqual (186, *(scan + 49441), "49441");
						Assert.AreEqual (210, *(scan + 50450), "50450");
						Assert.AreEqual (215, *(scan + 51459), "51459");
						Assert.AreEqual (155, *(scan + 52468), "52468");
						Assert.AreEqual (56, *(scan + 53477), "53477");
						Assert.AreEqual (149, *(scan + 54486), "54486");
						Assert.AreEqual (137, *(scan + 55495), "55495");
						Assert.AreEqual (141, *(scan + 56504), "56504");
						Assert.AreEqual (36, *(scan + 57513), "57513");
						Assert.AreEqual (39, *(scan + 58522), "58522");
						Assert.AreEqual (25, *(scan + 59531), "59531");
						Assert.AreEqual (44, *(scan + 60540), "60540");
						Assert.AreEqual (12, *(scan + 61549), "61549");
						Assert.AreEqual (161, *(scan + 62558), "62558");
						Assert.AreEqual (179, *(scan + 63567), "63567");
						Assert.AreEqual (181, *(scan + 64576), "64576");
						Assert.AreEqual (165, *(scan + 65585), "65585");
						Assert.AreEqual (182, *(scan + 66594), "66594");
						Assert.AreEqual (186, *(scan + 67603), "67603");
						Assert.AreEqual (201, *(scan + 68612), "68612");
						Assert.AreEqual (49, *(scan + 69621), "69621");
						Assert.AreEqual (161, *(scan + 70630), "70630");
						Assert.AreEqual (140, *(scan + 71639), "71639");
						Assert.AreEqual (2, *(scan + 72648), "72648");
						Assert.AreEqual (15, *(scan + 73657), "73657");
						Assert.AreEqual (33, *(scan + 74666), "74666");
						Assert.AreEqual (17, *(scan + 75675), "75675");
						Assert.AreEqual (0, *(scan + 76684), "76684");
						Assert.AreEqual (47, *(scan + 77693), "77693");
						Assert.AreEqual (4, *(scan + 78702), "78702");
						Assert.AreEqual (142, *(scan + 79711), "79711");
						Assert.AreEqual (151, *(scan + 80720), "80720");
						Assert.AreEqual (124, *(scan + 81729), "81729");
						Assert.AreEqual (81, *(scan + 82738), "82738");
						Assert.AreEqual (214, *(scan + 83747), "83747");
						Assert.AreEqual (217, *(scan + 84756), "84756");
						Assert.AreEqual (30, *(scan + 85765), "85765");
						Assert.AreEqual (185, *(scan + 86774), "86774");
						Assert.AreEqual (200, *(scan + 87783), "87783");
						Assert.AreEqual (37, *(scan + 88792), "88792");
						Assert.AreEqual (2, *(scan + 89801), "89801");
						Assert.AreEqual (41, *(scan + 90810), "90810");
						Assert.AreEqual (16, *(scan + 91819), "91819");
						Assert.AreEqual (0, *(scan + 92828), "92828");
						Assert.AreEqual (146, *(scan + 93837), "93837");
						Assert.AreEqual (163, *(scan + 94846), "94846");
#endif
					}
				}
				finally {
					bmp.UnlockBits (data);
				}
			}
		}

		[Test]
		public void Save () 
		{				
			string sOutFile =  "linerect" + getOutSufix() + ".jpeg";
						
			// Save		
			Bitmap bmp = new Bitmap (100, 100, PixelFormat.Format32bppRgb);						
			Graphics gr = Graphics.FromImage (bmp);

			using (Pen p = new Pen (Color.Red, 2)) {
				gr.DrawLine (p, 10.0F, 10.0F, 90.0F, 90.0F);
				gr.DrawRectangle (p, 10.0F, 10.0F, 80.0F, 80.0F);
			}

			try {
				bmp.Save (sOutFile, ImageFormat.Bmp);

				// Load			
				using (Bitmap bmpLoad = new Bitmap (sOutFile)) {
					Color color = bmpLoad.GetPixel (10, 10);
					Assert.AreEqual (Color.FromArgb (255, 255, 0, 0), color);
				}
			}
			finally {
				gr.Dispose ();
				bmp.Dispose ();
				try {
					File.Delete (sOutFile);
				}
				catch {
				}
			}
		}
	}
}
