package org.maplibre.android.style.layers;

import androidx.annotation.ColorInt;
import androidx.annotation.Keep;
import androidx.annotation.NonNull;

public class TrackLayer extends CustomLayer {

  private long state;

  public TrackLayer(@NonNull String id) {
    this(id, nativeCreateState());
  }

  private TrackLayer(String id, long state) {
    super(id, nativeCreateHost(state));
    this.state = state;
  }

  public void setTrack(@NonNull double[] lonLatAltitudes, @NonNull int[] colors) {
    if (state != 0) {
      nativeSetTrack(state, lonLatAltitudes, colors);
    }
  }

  public void setStyle(float widthPixels, float altitudeScale) {
    if (state != 0) {
      nativeSetStyle(state, widthPixels / 2f, altitudeScale);
    }
  }

  public void setLineVisible(boolean visible) {
    if (state != 0) {
      nativeSetLineVisible(state, visible);
    }
  }

  public void setCursor(double longitude, double latitude, double altitude) {
    if (state != 0) {
      nativeSetCursor(state, longitude, latitude, altitude);
    }
  }

  public void clearCursor() {
    if (state != 0) {
      nativeClearCursor(state);
    }
  }

  public void setCursorStyle(float diameterPixels, float outlineWidthPixels,
                             @ColorInt int fillColor, @ColorInt int outlineColor) {
    if (state != 0) {
      nativeSetCursorStyle(state, diameterPixels, outlineWidthPixels, fillColor, outlineColor);
    }
  }

  public void release() {
    if (state != 0) {
      nativeReleaseState(state);
      state = 0;
    }
  }

  @Keep
  private static native long nativeCreateState();

  @Keep
  private static native long nativeCreateHost(long state);

  @Keep
  private static native void nativeReleaseState(long state);

  @Keep
  private static native void nativeSetTrack(long state, double[] lonLatAltitudes, int[] colors);

  @Keep
  private static native void nativeSetStyle(long state, float halfWidth, float altitudeScale);

  @Keep
  private static native void nativeSetLineVisible(long state, boolean visible);

  @Keep
  private static native void nativeSetCursor(long state, double longitude, double latitude, double altitude);

  @Keep
  private static native void nativeClearCursor(long state);

  @Keep
  private static native void nativeSetCursorStyle(long state, float diameter, float outlineWidth,
                                                  int fillColor, int outlineColor);
}
