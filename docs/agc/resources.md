# Resources: V#, T#, S#

Descriptors reach a shader through user data or through tables in memory
that user data points at. The translator resolves them while it walks the
scalar code (see [shaders.md](shaders.md)); the decoding below is in
`gen5_manifest.h` and `gen5_emit_image.h`.

## V# - buffers, four dwords

| field | where |
| --- | --- |
| base address | dword 0, plus dword 1 bits 0-15 - 48 bits |
| stride | dword 1 bits 16-29 |
| records | dword 2 |

Size in bytes is `records` when the stride is zero and `records * stride`
otherwise. An address under the first page is not a buffer. **measured**

Descriptors whose base is not 16-byte aligned are ordinary (the intro's
large double-buffered buffers sit at 0x553F41DD0 and 0x554F41DD0), and a V#
may cover far more than the shader reads - 16 MB ranges over per-frame rings
are common. **measured**

## T# - images, eight dwords

| field | where |
| --- | --- |
| base address | (dword 0, plus dword 1 bits 0-7) << 8 - 256-byte units |
| format | dword 1 bits 20-28 - the "unified" format number below |
| width - 1 | dword 1 bits 30-31, plus dword 2 bits 0-13 - 14 bits |
| height - 1 | dword 2 bits 14-29 |
| dst_sel x/y/z/w | dword 3, three bits each from bit 0: 0 zero, 1 one, 4-7 x/y/z/w |
| tile mode | dword 3 bits 20-24 |
| type | dword 3 bits 28-31 |
| depth - 1 | dword 4 bits 0-12, for volumes |

**measured**

Types 1-7 are not images; a descriptor with one of them, a zero format, an
address under the first page or a width or height of one is not bound as an
image. Binding two such "images" in one dispatch put writes at addresses
nothing had allocated and killed the process inside the host driver.
**measured**

Type 10 is a 3D image. Treated as its first slice, the froxel fog volume
(240x135x64) and the 48x27x64 lighting tables lost their third coordinate.
They are 3D images with 3D views, slice count from dword 4. **measured**
(`c3ce36f`)

### Single-texel images

A 1x1 image at a real address is a material default - a white mask, a flat
normal. It fails the image test above, so the translation reads its one
texel and uses it as a constant, with the swizzle applied. Formats 56 and 1
are read from the texel; anything else reads white, which is what a default
texture usually is. **measured**

## S# - samplers, four dwords

Filters come from dword 2 (magnification at bits 20-21). **measured**; the
rest of the sampler is decoded in `ps5gpu_native.cpp` and has not needed
attention.

## Formats

The T# format field numbers formats in GFX10's unified numbering. The ones
this title uses, and their Vulkan equivalents:

| unified | Vulkan |
| --- | --- |
| 1, 2, 5, 6 | R8 UNORM, SNORM, UINT, SINT |
| 7, 8, 11, 12, 13 | R16 UNORM, SNORM, UINT, SINT, SFLOAT |
| 14, 15, 18, 19 | R8G8 UNORM, SNORM, UINT, SINT |
| 20, 21, 22 | R32 UINT, SINT, SFLOAT |
| 23, 24, 27, 28, 29 | R16G16 UNORM, SNORM, UINT, SINT, SFLOAT |
| 36 | B10G11R11 UFLOAT |
| 50, 54 | A2B10G10R10 UNORM, UINT |
| 56, 57, 60, 61 | R8G8B8A8 UNORM, SNORM, UINT, SINT |
| 62, 63, 64 | R32G32 UINT, SINT, SFLOAT |
| 65, 66, 69, 70, 71 | R16G16B16A16 UNORM, SNORM, UINT, SINT, SFLOAT |
| 75, 76, 77 | R32G32B32A32 UINT, SINT, SFLOAT |
| 130 | R8G8B8A8 SRGB |
| 132 | E5B9G9R9 UFLOAT |
| 133 | B5G6R5 UNORM |
| 134 | R5G5B5A1 UNORM |
| 136 | R4G4B4A4 UNORM |
| 169, 170 | BC1 UNORM, SRGB |
| 171, 172 | BC2 UNORM, SRGB |
| 173, 174 | BC3 UNORM, SRGB |
| 175, 176 | BC4 UNORM, SNORM |
| 177, 178 | BC5 UNORM, SNORM |
| 179, 180 | BC6H UFLOAT, SFLOAT |
| 181, 182 | BC7 UNORM, SRGB |

**measured** for every format the title was seen to use; the rest of the
table is the GFX10 numbering applied consistently. BC blocks are eight bytes
for BC1 and BC4, sixteen for the others.

"Sony Interactive Entertainment presents" is a 1600x256 BC7_SRGB texture in
tile mode 9. With only BC6H mapped, it drew as a black rectangle.
**measured** (`c4f8b3a`)

Keep one table. There were three copies of format facts in the runtime, one
knowing six formats against forty-four in another, and an unlisted format
made uploads return without a word: 226 image bindings in one frame received
no data. **measured**

## Tile modes

| mode | meaning here |
| --- | --- |
| 0 | Linear |
| 4, 8, 16, 20, 24 | Z-order ("Z swizzle") modes - depth buffers |
| 9 | SW_64KB_S: the 4 KB standard pattern, continued with Y and X alternating |

Detiling for mode 9 exists for eight- and sixteen-byte elements. **measured**
(`c4f8b3a`). Other modes are detiled as they were met; see `detile_*` in
`ps5gpu_native.cpp`. A mode the runtime cannot detile uploads nothing
rather than garbage.

## Mip chains in swizzled images

A swizzled image stores its mip chain smallest level first and the full-size
level last. The top level's offset is the sum of the smaller levels, from
the base and last level in the descriptor, with the mip tail sharing one
block. Read from the base address, the SIE plate came out as its own mip
chain - a row of ever larger copies. **measured** (`0dc8b53`)

## Depth buffers and cleared memory

The console clears depth through HTILE metadata and leaves the bytes as they
were. A 32-bit float image in a Z swizzle mode that nothing on the device
drew into is therefore read as cleared - 1.0, the far plane - whatever its
memory holds. Read from memory, the intro's fog pass took leftover bytes for
geometry and drew a magenta and blue wedge. **measured** (`0dc8b53`,
`2566676`)

## Render targets sampled as textures

A target the GPU drew into lives only in the runtime's surface; guest memory
behind it stays zero. Uploading from guest memory erases the frame's own
work - 65 of 70 uploads in one run were zeros, and a bloom chain sampled
black. A texture whose guest bytes are all zero and which has a surface is
never uploaded over that surface, whether or not it has been drawn into
yet. **measured**
