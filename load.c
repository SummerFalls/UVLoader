/*
 * load.c - Parses and loads an ELF to memory
 * Copyright 2012 Yifan Lu
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "load.h"
#include "relocate.h"
#include "resolve.h"
#include "scefuncs.h"
#include "utils.h"
#include "uvloader.h"

static module_imports_t *g_import_start;
static void *g_import_end;

/********************************************//**
 *  \brief Loads file to memory
 *  
 *  \returns Zero on success, otherwise error
 ***********************************************/
int
uvl_load_file (const char *filename,    ///< File to load
                     void **data,       ///< Output pointer to data
                  PsvSSize *size)       ///< Output pointer to data size
{
    PsvUID fd;
    PsvUID memblock;
    char *base;
    PsvOff filesz;
    PsvOff nread;
    PsvSSize nbytes;

    fd = sceIoOpen (filename, PSP2_O_RDONLY, 0);
    if (fd < 0)
    {
        LOG ("Failed to open %s for reading.", filename);
        return -1;
    }
    filesz = sceIoLseek (fd, 0LL, PSP2_SEEK_END);
    if (filesz < 0)
    {
        LOG ("Failed to find file size: 0x%X", filesz);
        return -1;
    }
    sceIoLseek (fd, 0LL, PSP2_SEEK_SET);
    memblock = sceKernelAllocMemBlock ("UVLTemp", 0xC20D060, (filesz + 0xFFF) & ~0xFFF, NULL);
    if (memblock < 0)
    {
        LOG ("Failed allocate %u bytes of memory.", memblock);
        return -1;
    }
    if (sceKernelGetMemBlockBase (memblock, &base) < 0)
    {
        LOG ("Failed to locate base for block 0x%08X.", memblock);
        return -1;
    }
    base = (char *)(((u32_t)base + 0xFFF) & ~0xFFF); // align memory base
    nbytes = 0;
    while ((nread = sceIoRead (fd, base+nbytes, filesz)) < filesz-nbytes)
    {
        nbytes += nread;
    }
    if (nbytes < 0)
    {
        LOG ("Failed to read %s: 0x%08X", filename, nbytes);
        return -1;
    }
    IF_DEBUG LOG ("Read %u bytes from %s", nbytes, filename);
    if (sceIoClose (fd) < 0)
    {
        LOG ("Failed to close file.");
        return -1;
    }

    *data = base;
    *size = nbytes;

    return 0;
}

/********************************************//**
 *  \brief Frees data pointer created by load
 *  
 *  \returns Zero on success, otherwise error
 ***********************************************/
static inline int
uvl_free_data (void *data)      ///< Data allocated by @c uvl_load_file
{
    PsvUID block;

    block = sceKernelFindMemBlockByAddr (data, 0);
    if (block < 0)
    {
        LOG ("Cannot find block id: 0x%08X", block);
    }
    if (sceKernelFreeMemBlock (block) < 0)
    {
        LOG ("Cannot free block: 0x%08X", block);
        return -1;
    }
    return 0;
}

/********************************************//**
 *  \brief Loads an supported executable
 *  
 *  This function identifies and loads a 
 *  executable at the given file.
 *  Currently supports ELF and SCE executable.
 *  \returns Zero on success, otherwise error
 ***********************************************/
int
uvl_load_exe (const char *filename, ///< Absolute path to executable
                    void **entry,   ///< Returned pointer to entry pointer
            uvl_loaded_t *loaded)   ///< Pointer to loaded info
{
    void *data;
    PsvSSize size;
    char *magic;
    u32_t offset;

    *entry = NULL;
    IF_DEBUG LOG ("Opening %s for reading.", filename);
    if (uvl_load_file (filename, &data, &size) < 0)
    {
        LOG ("Cannot load file.");
        return -1;
    }

    magic = (char*)data;
    IF_VERBOSE LOG ("Magic number: 0x%02X 0x%02X 0x%02X 0x%02X", magic[0], magic[1], magic[2], magic[3]);

    if (magic[0] == ELFMAG0)
    {
        if (magic[1] == ELFMAG1 && magic[2] == ELFMAG2 && magic[3] == ELFMAG3)
        {
            IF_DEBUG LOG ("Found a ELF, loading.");
            if (uvl_load_elf (data, entry, loaded) < 0)
            {
                LOG ("Cannot load ELF.");
                return -1;
            }
        }
    }
    else if (magic[0] == SCEMAG0)
    {
        if (magic[1] == SCEMAG1 && magic[2] == SCEMAG2 && magic[3] == SCEMAG3)
        {
            offset = ((u32_t*)data)[4];
            IF_DEBUG LOG ("Loading FSELF. ELF offset at 0x%08X", offset);
            if (uvl_load_elf ((void*)((u32_t)data + offset), entry, loaded) < 0)
            {
                LOG ("Cannot load FSELF.");
                return -1;
            }
        }
    }
    else
    {
        LOG ("Invalid magic.");
        return -1;
    }

    // free data
    if (uvl_free_data (data) < 0)
    {
        LOG ("Cannot free data");
        return -1;
    }
    return 0;
}

/********************************************//**
 *  \brief Loads an ELF file
 *  
 *  Performs both loading and resolving NIDs
 *  \returns Zero on success, otherwise error
 ***********************************************/
int 
uvl_load_elf (void *data,           ///< ELF data start
              void **entry,         ///< Returned pointer to entry pointer
            uvl_loaded_t *loaded)   ///< Pointer to loaded info
{
    Elf32_Ehdr_t *elf_hdr;
    u32_t i;
    int addend;
    *entry = NULL;

    // get headers
    IF_VERBOSE LOG ("Reading headers.");
    elf_hdr = data;
    IF_DEBUG LOG ("Checking headers.");
    if (uvl_elf_check_header (elf_hdr) < 0)
    {
        LOG ("Check header failed.");
        return -1;
    }

    // get program headers
    Elf32_Phdr_t *prog_hdrs;
    IF_VERBOSE LOG ("Reading program headers.");
    prog_hdrs = (void*)((u32_t)data + elf_hdr->e_phoff);
    loaded->numsegs = elf_hdr->e_phnum;
    if (sizeof (PsvUID) * loaded->numsegs + sizeof (*loaded) > LOADED_INFO_SIZE)
    {
        LOG ("Too many segments: %d", elf_hdr->e_phnum);
        return -1;
    }

    // actually load the ELF
    PsvUID memblock;
    void *blockaddr;
    u32_t length;
    if (elf_hdr->e_phnum < 1)
    {
        LOG ("No program sections to load!");
        return -1;
    }
    IF_DEBUG LOG ("Loading %u program segments.", elf_hdr->e_phnum);
    for (i = 0; i < elf_hdr->e_phnum; i++)
    {
        if (prog_hdrs[i].p_type == PT_LOAD)
        {
            length = prog_hdrs[i].p_memsz;
            length = (length + 0xFFFFF) & ~0xFFFFF; // Align to 1MB
            if (prog_hdrs[i].p_flags & PF_X == PF_X) // executable section
            {
                memblock = sceKernelAllocCodeMemBlock ("UVLHomebrew", length);
            }
            else // data section
            {
                memblock = sceKernelAllocMemBlock ("UVLHomebrew", 0xC20D060, length, NULL);
            }
            if (memblock < 0)
            {
                LOG ("Error allocating memory. 0x%08X", memblock);
                return -1;
            }
            if (sceKernelGetMemBlockBase (memblock, &blockaddr) < 0)
            {
                LOG ("Error getting memory block address.");
            }

            // remember where we're loaded
            loaded->segs[i] = memblock;
            prog_hdrs[i].p_vaddr = blockaddr;

            IF_DEBUG LOG ("Allocated memory at 0x%08X, attempting to load segment %u.", (u32_t)blockaddr, i);
            uvl_segment_write (&prog_hdrs[i], 0, (void*)((u32_t)data + prog_hdrs[i].p_offset), prog_hdrs[i].p_filesz);
            uvl_unlock_mem ();
            memset ((void*)((u32_t)blockaddr + prog_hdrs[i].p_filesz), 0, prog_hdrs[i].p_memsz - prog_hdrs[i].p_filesz);
            uvl_lock_mem ();
        }
        else if (prog_hdrs[i].p_type == PT_SCE_RELA)
        {
            uvl_relocate ((void*)((u32_t)data + prog_hdrs[i].p_offset), prog_hdrs[i].p_filesz, prog_hdrs);
        }
        else
        {
            IF_DEBUG LOG ("Segment %u is not loadable. Skipping.", i);
            continue;
        }
    }

    // get mod_info
    module_info_t *mod_info;
    int idx;
    IF_DEBUG LOG ("Getting module info.");
    if ((idx = uvl_elf_get_module_info (elf_hdr, prog_hdrs, &mod_info)) < 0)
    {
        LOG ("Cannot find module info section.");
        return -1;
    }
    IF_DEBUG LOG ("Module name: %s, export table offset: 0x%08X, import table offset: 0x%08X", mod_info->modname, mod_info->ent_top, mod_info->stub_top);

    // resolve NIDs
    uvl_unlock_mem ();
    g_import_start = (void*)(prog_hdrs[idx].p_vaddr + mod_info->stub_top);
    g_import_end = (void*)(prog_hdrs[idx].p_vaddr + mod_info->stub_end);
    uvl_lock_mem ();
    
    module_imports_t *import = g_import_start;
    void *end = g_import_end;

    for (; (void *)import < end; import = IMP_GET_NEXT (import))
    {
        IF_DEBUG LOG ("Resolving imports for %s", IMP_GET_NAME (import));
        if (uvl_resolve_imports (import) < 0)
        {
            LOG ("Failed to resolve imports for %s", IMP_GET_NAME (import));
            return -1;
        }
    }

    // find the entry point
    *entry = (void*)(prog_hdrs[idx].p_vaddr + mod_info->mod_start);
    if (*entry == NULL)
    {
        LOG ("Invalid module entry function.\n");
        return -1;
    }

    return 0;
}

int
uvl_resolve_import_by_name(const char *name)
{
    module_imports_t *import = g_import_start;
    void *end = g_import_end;

    for (; (void *)import < end; import = IMP_GET_NEXT (import))
    {
        if (strcmp(name, IMP_GET_NAME (import)) == 0) {
            IF_DEBUG LOG ("Resolving imports for %s", IMP_GET_NAME (import));
            if (uvl_resolve_imports (import) < 0)
            {
                LOG ("Failed to resolve imports for %s", IMP_GET_NAME (import));
                return -1;
            }

            return 0;
        }
    }

    return -1;
}

/********************************************//**
 *  \brief Validates ELF header
 *  
 *  Makes sure the ELF is recognized by the 
 *  Vita's architecture.
 *  \returns Zero if valid, otherwise invalid
 ***********************************************/
int 
uvl_elf_check_header (Elf32_Ehdr_t *hdr) ///< ELF header to check
{
    IF_VERBOSE LOG ("Magic number: 0x%02X 0x%02X 0x%02X 0x%02X", hdr->e_ident[EI_MAG0], hdr->e_ident[EI_MAG1], hdr->e_ident[EI_MAG2], hdr->e_ident[EI_MAG3]);
    // magic number
    if (!(hdr->e_ident[EI_MAG0] == ELFMAG0 && hdr->e_ident[EI_MAG1] == ELFMAG1 && hdr->e_ident[EI_MAG2] == ELFMAG2 && hdr->e_ident[EI_MAG3] == ELFMAG3))
    {
        LOG ("Invalid ELF magic number.");
        return -1;
    }
    // class
    if (!(hdr->e_ident[EI_CLASS] == ELFCLASS32))
    {
        LOG ("Not a 32bit executable.");
        return -1;
    }
    // data
    if (!(hdr->e_ident[EI_DATA] == ELFDATA2LSB))
    {
        LOG ("Not a valid ARM executable.");
        return -1;
    }
    // version
    if (!(hdr->e_ident[EI_VERSION] == EV_CURRENT))
    {
        LOG ("Unsupported ELF version.");
        return -1;
    }
    // type
    if (!hdr->e_type == ET_SCE_RELEXEC)
    {
        LOG ("Only ET_SCE_RELEXEC files are supported.");
        return -1;
    }
    // machine
    if (!(hdr->e_machine == EM_ARM))
    {
        LOG ("Not an ARM executable.");
        return -1;
    }
    // version
    if (!(hdr->e_version == EV_CURRENT))
    {
        LOG ("Unsupported ELF version.");
        return -1;
    }
    return 0;
}

/********************************************//**
 *  \brief Finds SCE module info
 *  
 *  This finds the module_info_t for this SCE 
 *  ELF.
 *  \returns -1 on error, index of segment 
 *      containing module info on success.
 ***********************************************/
int 
uvl_elf_get_module_info (Elf32_Ehdr_t *elf_hdr, ///< ELF header
                 Elf32_Phdr_t *elf_phdrs,       ///< ELF program headers
                module_info_t **mod_info)       ///< Where to read information to
{
    u32_t offset;
    u32_t index;

    index = ((u32_t)elf_hdr->e_entry & 0xC0000000) >> 30;
    offset = (u32_t)elf_hdr->e_entry & 0x3FFFFFFF;

    if (elf_phdrs[index].p_vaddr == NULL)
    {
        LOG ("Invalid segment index %d\n", index);
        return -1;
    }

    *mod_info = (module_info_t *)((char *)elf_phdrs[index].p_vaddr + offset);

    return index;
}

// No longer needed for relocatable ELFs
#if 0
/********************************************//**
 *  \brief Frees memory of where we want to load
 *  
 *  Finds the max and min addresses we want to
 *  load to using program headers and frees 
 *  any module taking up those spaces.
 *  \returns Zero on success, otherwise error
 ***********************************************/
int
uvl_elf_free_memory (Elf32_Phdr_t *prog_hdrs,   ///< Array of program headers
                              int count)        ///< Number of program headers
{
    void *min_addr = (void*)0xFFFFFFFF;
    void *max_addr = (void*)0x00000000;
    loaded_module_info_t m_mod_info;
    PsvUID mod_list[MAX_LOADED_MODS];
    u32_t num_loaded = MAX_LOADED_MODS;
    int i, j;
    u32_t length;
    int temp[2];

    IF_VERBOSE LOG ("Reading %u program headers.", count);
    for (i = 0; i < count; i++)
    {
        if (prog_hdrs[i].p_vaddr < min_addr)
        {
            min_addr = prog_hdrs[i].p_vaddr;
        }
        if ((u32_t)prog_hdrs[i].p_vaddr + prog_hdrs[i].p_memsz > (u32_t)max_addr)
        {
            max_addr = (void*)((u32_t)prog_hdrs[i].p_vaddr + prog_hdrs[i].p_memsz);
        }
    }
    IF_DEBUG LOG ("Lowest load address: 0x%08X, highest: 0x%08X", (u32_t)min_addr, (u32_t)max_addr);\

    IF_DEBUG LOG ("Getting list of loaded modules.");
    if (sceKernelGetModuleList (0xFF, mod_list, &num_loaded) < 0)
    {
        LOG ("Failed to get module list.");
        return -1;
    }
    IF_DEBUG LOG ("Found %u loaded modules.", num_loaded);
    for (i = 0; i < num_loaded; i++)
    {
        m_mod_info.size = sizeof (loaded_module_info_t); // should be 440
        IF_VERBOSE LOG ("Getting information for module #%u, UID: 0x%X.", i, mod_list[i]);
        if (sceKernelGetModuleInfo (mod_list[i], &m_mod_info) < 0)
        {
            LOG ("Error getting info for mod 0x%08X, continuing", mod_list[i]);
            continue;
        }
        for (j = 0; j < 3; j++)
        {
            //if (m_mod_info.segments[j].vaddr > min_addr || (u32_t)m_mod_info.segments[j].vaddr + m_mod_info.segments[j].memsz > (u32_t)min_addr)
            if (m_mod_info.segments[j].vaddr == (void*)0x81000000)
            {
                IF_DEBUG LOG ("Module %s segment %u (0x%08X, size %u) is in our address space. Attempting to unload.", m_mod_info.module_name, j, (u32_t)m_mod_info.segments[j].vaddr, m_mod_info.segments[j].memsz);
                if (sceKernelStopUnloadModule (mod_list[i], 0, 0, 0, &temp[0], &temp[1]) < 0)
                {
                    LOG ("Error unloading %s.", m_mod_info.module_name);
                    return -1;
                }
                break;
            }
        }
    }
    return 0;
}
#endif
