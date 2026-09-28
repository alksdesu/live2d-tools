`adobe-plain-raster.psd` is a 64 × 64 generated test image with an empty top layer, opened and saved in Adobe Photoshop 2022. It contains the editor's ordinary raster metadata, including a nonempty `shmd/cust/layerTime` descriptor. Both layers use normal blending.

File SHA-256: `3fc03e13d36eea6f4b66ea6c32ff0c05bdaebff66ccf78f76ecfa44cf7f0b7d0`.

The nonempty layer's straight RGBA bytes have SHA-256 `ce7c5fcbc9d2d7ca525c98822902e2735ec9c93cd59380ad723a64b290dffcd7`, independently checked before and after the Photoshop save. The empty layer retains its name and ID and has zero decoded samples.
