#pragma once

class HalFile;
class Print;

class BmpToBmpConverter {
  static bool bmpFileToBmpStreamInternal(HalFile& bmpFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool forceOneBit, bool crop, bool originalThresholds);

 public:
  // Scales a BMP to the portrait display the way book covers are converted: crop or fit, then one Atkinson
  // pass into a 2-bit BMP. 1-bit sources stay 1-bit so B/W images keep the B/W refresh path.
  static bool bmpFileToBmpStream(HalFile& bmpFile, Print& bmpOut, bool crop, bool originalThresholds);
  // Scales/crops a BMP to exact target bounds and converts to a strict 1-bit Atkinson BMP.
  static bool bmpFileTo1BitBmpStreamWithSize(HalFile& bmpFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                             bool crop = true);
};
