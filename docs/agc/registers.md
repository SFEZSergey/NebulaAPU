# Registers

Offsets are dword offsets within their space - SH (shader), context (CX) or
UCONFIG (UC) - with the selector nibble already stripped (see
[command-stream.md](command-stream.md#register-offsets-carry-a-selector)).
Names follow the GFX10 register names where the meaning matched what we saw.
The runtime keeps one map per space and reads from them at each draw or
dispatch.

## Shader programs (SH)

| offset | register | notes |
| --- | --- | --- |
| 0x08 / 0x09 | SPI_SHADER_PGM_LO/HI_PS | Pixel shader address, 256-byte units |
| 0x0C | PS user data, first register | |
| 0x48 / 0x49 | SPI_SHADER_PGM_LO/HI_VS | |
| 0x4C | VS user data | |
| 0x88 / 0x89 / 0x8A | SPI_SHADER_PGM_LO/HI/RSRC1_GS | |
| 0x8C | GS user data | |
| 0xC8 / 0xC9 | SPI_SHADER_PGM_LO/HI_ES | The vertex stage this title uses |
| 0xCC | ES user data | Lands in s8 upward in the vertex stage |
| 0x108 / 0x109 / 0x10A | SPI_SHADER_PGM_LO/HI/RSRC1_HS | |
| 0x148 / 0x149 | SPI_SHADER_PGM_LO/HI_LS | |

Which user-data register a vertex stage starts from is chosen per draw from
what the stream actually set, not fixed. **measured**

Up to 32 user-data registers are taken for a graphics stage. The stage's
resource descriptor declares a count, and trusting it resolved fewer loads
(26% against 31%); the descriptors the unresolved loads wanted sit at s16,
s20, s24 and s28, above sixteen. Stop at the first register the draw did not
set rather than skip it - skipping shifts every later one down a slot and
every address computed from them goes wrong silently. **measured**

## Compute (SH)

| offset | register | notes |
| --- | --- | --- |
| 0x204-0x206 | COMPUTE_START_X/Y/Z | |
| 0x207-0x209 | COMPUTE_NUM_THREAD_X/Y/Z | Workgroup shape - becomes the module's LocalSize |
| 0x20C / 0x20D | COMPUTE_PGM_LO/HI | |
| 0x213 | COMPUTE_PGM_RSRC2 | Bits 7, 8, 9 enable the workgroup id X, Y, Z SGPRs |
| 0x240 | Compute user data, first register | |

The enabled workgroup ids occupy the SGPRs right after the user data, in
order X, Y, Z. Local invocation ids arrive in v0, v1, v2. A translation that
ignored both ran every invocation of every workgroup as thread zero of group
zero - the fog volume's scattering pass wrote its first voxel 4080 times and
left the rest empty. **measured** (`8cc6b71`)

## Pixel shader inputs (CX)

| offset | register | notes |
| --- | --- | --- |
| 0x191-0x1B0 | SPI_PS_INPUT_CNTL_0..31 | Written through the 0x10000000 selector |
| 0x1B3 | SPI_PS_INPUT_ENA | Which system values the pixel shader is given |
| 0x1B4 | SPI_PS_INPUT_ADDR | And in which registers |

`SPI_PS_INPUT_CNTL_n`: bits 0-4 name the vertex-stage export attribute n
reads; bit 5 selects a constant instead (the constant in bits 8-9); bit 10
is flat shading. Translated without them, attribute N reads export N.
**measured**

## Colour targets (CX)

| offset | register | notes |
| --- | --- | --- |
| 0x08E | CB_TARGET_MASK | Four bits per slot |
| 0x08F | CB_SHADER_MASK | Which slots the pixel shader exports |
| 0x1E0 + n | CB_BLEND0..7_CONTROL | |
| 0x202 | CB_COLOR_CONTROL | Bits 4-6: mode; 2 and 6 are metadata passes |
| 0x318 + 15n | CB_COLORn_BASE | Low 32 bits of address >> 8 |
| 0x31C + 15n | CB_COLORn_INFO | Format, number type |
| 0x323 / 0x324 | CB_COLOR0_CLEAR_WORD0/1 | The fast-clear value |
| 0x390 + n | CB_COLORn_BASE_EXT | High address bits |
| 0x3B0 + n | CB_COLORn_ATTRIB2 | Width, height |
| 0x3B8 + n | CB_COLORn_ATTRIB3 | Tile mode, and the rest |

A colour target record is sixteen consecutive values; decode all of them. An
attempt to take only the four the validator reads broke the frame.
**measured**

A draw whose target mask and shader mask share no channel writes no colour
at all however complete the rest looks - about 7 of every 31 draws on the
loading screen are like this and are skipped. **measured**

## Viewport and scissor (CX)

| offset | register |
| --- | --- |
| 0x00C / 0x00D | PA_SC_SCREEN_SCISSOR_TL/BR |
| 0x080 | PA_SC_WINDOW_OFFSET |
| 0x081 / 0x082 | PA_SC_WINDOW_SCISSOR_TL/BR |
| 0x090 / 0x091 | PA_SC_GENERIC_SCISSOR_TL/BR |
| 0x094 / 0x095 | PA_SC_VPORT_SCISSOR_0_TL/BR |
| 0x0B4 / 0x0B5 | PA_SC_VPORT_ZMIN/ZMAX_0 |
| 0x10F-0x112 | PA_CL_VPORT_XSCALE/XOFFSET/YSCALE/YOFFSET |
| 0x292 | PA_SC_MODE_CNTL_0 |

Viewports often arrive inside an indirect register table, several in a row,
and each one wins over the one before as on hardware. Reading only the
first left a previous pass's viewport in place. **measured**

## Primitive and index state (UC)

| offset | register |
| --- | --- |
| 0x242 | VGT_PRIMITIVE_TYPE |
| 0x24A | GE_INDEX_OFFSET |

Half of this title's draws are rect lists: three vertices describing a
rectangle whose fourth corner is v0 + v2 - v1. Vulkan has no rect list.
**inferred** - drawn here as a strip with a derived fourth vertex; the other
reading we compared against uses a patch list and tessellation. Whether ours
is right in every case is **open**.
