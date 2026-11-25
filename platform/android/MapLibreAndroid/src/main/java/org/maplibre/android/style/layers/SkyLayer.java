package org.maplibre.android.style.layers;

import androidx.annotation.ColorInt;
import androidx.annotation.Keep;
import androidx.annotation.NonNull;

public class SkyLayer extends CustomLayer {

  public SkyLayer(@NonNull String id, @ColorInt int haze, @ColorInt int light, @ColorInt int zenith,
                  float lightDegrees, float zenithDegrees) {
    super(id, nativeCreateHost(haze, light, zenith, lightDegrees, zenithDegrees));
  }

  @Keep
  private static native long nativeCreateHost(int haze, int light, int zenith,
                                              float lightDegrees, float zenithDegrees);
}
