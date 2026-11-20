"""Embed a Vulkan cache captured from the Switch renderer, without altering it."""
from pathlib import Path
import struct
import sys

source,output=map(Path,sys.argv[1:])
data=source.read_bytes()
if not 32<=len(data)<=16*1024*1024:
    raise SystemExit('Vulkan pipeline cache must be between 32 bytes and 16 MiB')
header_size,version,_,_=struct.unpack_from('<IIII',data)
if header_size!=32 or version!=1:
    raise SystemExit('Expected a VkPipelineCacheHeaderVersionOne cache')
lines=['// Generated from VulkanPipelineCache.bin; do not edit.\n',
       'alignas(4) static const std::uint8_t embeddedPipelineCache[] = {\n']
for at in range(0,len(data),32):
    lines.append('    '+','.join(f'0x{byte:02x}' for byte in data[at:at+32])+',\n')
lines.append('};\n')
text=''.join(lines)
if not output.exists() or output.read_text()!=text:output.write_text(text,newline='\n')
