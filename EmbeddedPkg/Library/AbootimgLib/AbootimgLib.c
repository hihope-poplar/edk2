/** @file

  Copyright (c) 2013-2014, ARM Ltd. All rights reserved.<BR>
  Copyright (c) 2017, Linaro. All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <Library/AbootimgLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/ZLib.h>

#include <Protocol/Abootimg.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePathFromText.h>
#include <Protocol/LoadedImage.h>

#include <libfdt.h>

// Check Val (unsigned) is a power of 2 (has only one bit set)
#define IS_POWER_OF_2(Val)                (Val != 0 && ((Val & (Val - 1)) == 0))

// Offset in Kernel Image
#define KERNEL_SIZE_OFFSET                0x10
#define KERNEL_MAGIC_OFFSET               0x38
#define KERNEL_MAGIC                      "ARMd"

#define BOOTIMG_HEADER_BLOCKS             1
#define FDTIMG_HEADER_BLOCKS              1

#define DEFAULT_UNCOMPRESS_BUFFER_SIZE    (32 * 1024 * 1024)

#define IS_DEVICE_PATH_NODE(node,type,subtype) (((node)->Type == (type)) && ((node)->SubType == (subtype)))

typedef struct {
  MEMMAP_DEVICE_PATH                      Node1;
  EFI_DEVICE_PATH_PROTOCOL                End;
} MEMORY_DEVICE_PATH;

STATIC ABOOTIMG_PROTOCOL                 *mAbootimg;

STATIC CONST MEMORY_DEVICE_PATH MemoryDevicePathTemplate =
{
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_MEMMAP_DP,
      {
        (UINT8)(sizeof (MEMMAP_DEVICE_PATH)),
        (UINT8)((sizeof (MEMMAP_DEVICE_PATH)) >> 8),
      },
    }, // Header
    0, // StartingAddress (set at runtime)
    0  // EndingAddress   (set at runtime)
  }, // Node1
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  } // End
};

STATIC
EFI_STATUS
CheckKernelImageHeader (
  IN  VOID                  *Kernel
  )
{
  if (Kernel == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  /* Check magic number of uncompressed Kernel Image */
  if (AsciiStrnCmp ((VOID *)((UINTN)Kernel + KERNEL_MAGIC_OFFSET), KERNEL_MAGIC, 4) == 0) {
    return EFI_SUCCESS;
  }
  return EFI_INVALID_PARAMETER;
}

STATIC
EFI_STATUS
UncompressKernel (
  IN  VOID                 *Source,
  IN  UINTN                *SourceSize,
  OUT VOID                **Kernel,
  OUT UINTN                *KernelSize
  )
{
  INTN            err;

  *Kernel = AllocatePages (EFI_SIZE_TO_PAGES (DEFAULT_UNCOMPRESS_BUFFER_SIZE));
  if (*Kernel == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }
  *KernelSize = DEFAULT_UNCOMPRESS_BUFFER_SIZE;
  err = GzipDecompress (Source, SourceSize, *Kernel, KernelSize);
  if (err) {
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
GetImgSize (
  IN  VOID    *BootImg,
  OUT UINTN   *ImgSize
  )
{
  ANDROID_BOOTIMG_HEADER   *Header;

  Header = (ANDROID_BOOTIMG_HEADER *) BootImg;

  if (AsciiStrnCmp (Header->BootMagic, BOOT_MAGIC, BOOT_MAGIC_LENGTH) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  ASSERT (IS_POWER_OF_2 (Header->PageSize));

  /* Get real size of abootimg */
  *ImgSize = ALIGN_VALUE (Header->KernelSize, Header->PageSize) +
             ALIGN_VALUE (Header->RamdiskSize, Header->PageSize) +
             ALIGN_VALUE (Header->SecondStageBootloaderSize, Header->PageSize) +
             Header->PageSize;
  return EFI_SUCCESS;
}

EFI_STATUS
AbootimgGetKernelInfo (
  IN  VOID    *BootImg,
  OUT VOID   **Kernel,
  OUT UINTN   *KernelSize
  )
{
  ANDROID_BOOTIMG_HEADER   *Header;

  Header = (ANDROID_BOOTIMG_HEADER *) BootImg;

  if (AsciiStrnCmp (Header->BootMagic, BOOT_MAGIC, BOOT_MAGIC_LENGTH) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (Header->KernelSize == 0) {
    return EFI_NOT_FOUND;
  }

  ASSERT (IS_POWER_OF_2 (Header->PageSize));

  *KernelSize = Header->KernelSize;
  *Kernel = BootImg + Header->PageSize;
  return EFI_SUCCESS;
}

EFI_STATUS
GetRamdiskInfo (
  IN  VOID    *BootImg,
  OUT VOID   **Ramdisk,
  OUT UINTN   *RamdiskSize
  )
{
  ANDROID_BOOTIMG_HEADER   *Header;
  UINT8                    *BootImgBytePtr;

  // Cast to UINT8 so we can do pointer arithmetic
  BootImgBytePtr = (UINT8 *) BootImg;

  Header = (ANDROID_BOOTIMG_HEADER *) BootImg;

  if (AsciiStrnCmp (Header->BootMagic, BOOT_MAGIC, BOOT_MAGIC_LENGTH) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  ASSERT (IS_POWER_OF_2 (Header->PageSize));

  *RamdiskSize = Header->RamdiskSize;

  if (Header->RamdiskSize != 0) {
    *Ramdisk = (VOID *) (BootImgBytePtr + Header->PageSize +
                 ALIGN_VALUE (Header->KernelSize, Header->PageSize));
  }
  return EFI_SUCCESS;
}

EFI_STATUS
GetKernelArgs (
  IN  VOID    *BootImg,
  OUT CHAR8   *KernelArgs
  )
{
  ANDROID_BOOTIMG_HEADER   *Header;

  Header = (ANDROID_BOOTIMG_HEADER *) BootImg;
  AsciiStrnCpyS (KernelArgs, BOOTIMG_KERNEL_ARGS_SIZE, Header->KernelArgs,
    BOOTIMG_KERNEL_ARGS_SIZE);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
GetAttachedFdt (
  IN VOID                            *Kernel,
  OUT VOID                           **Fdt
  )
{
  UINTN                      RawKernelSize;
  INTN                       err;

  err = fdt_check_header ((VOID*)(UINTN)*Fdt);
  if (err == 0) {
    return EFI_SUCCESS;
  }

  // Get real kernel size.
  RawKernelSize = *(UINT32 *)((EFI_PHYSICAL_ADDRESS)(UINTN)Kernel + KERNEL_IMAGE_STEXT_OFFSET) +
                  *(UINT32 *)((EFI_PHYSICAL_ADDRESS)(UINTN)Kernel + KERNEL_IMAGE_RAW_SIZE_OFFSET);

  /* FDT is at the end of kernel image */
  *Fdt = (VOID *)((EFI_PHYSICAL_ADDRESS)(UINTN)Kernel + RawKernelSize);

  //
  // Sanity checks on the FDT blob.
  //
  err = fdt_check_header ((VOID*)(UINTN)*Fdt);
  if (err != 0) {
    Print (L"ERROR: Device Tree header not valid (err:%d)\n", err);
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
InstallFdt (
  IN     VOID                  *BootImg,
  IN     EFI_PHYSICAL_ADDRESS   FdtBase,
  IN OUT VOID                  *KernelArgs
  )
{
  VOID                      *Ramdisk;
  UINTN                      RamdiskSize;
  CHAR8                      ImgKernelArgs[BOOTIMG_KERNEL_ARGS_SIZE];
  INTN                       err;
  EFI_STATUS                 Status;
  EFI_PHYSICAL_ADDRESS       NewFdtBase;

  Status = gBS->LocateProtocol (&gAbootimgProtocolGuid, NULL, (VOID **) &mAbootimg);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = GetRamdiskInfo (
            BootImg,
            &Ramdisk,
            &RamdiskSize
            );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = GetKernelArgs (
            BootImg,
            ImgKernelArgs
            );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (ImgKernelArgs != NULL) {
    // Get kernel arguments from Android boot image
    AsciiStrToUnicodeStrS (ImgKernelArgs, KernelArgs, BOOTIMG_KERNEL_ARGS_SIZE);
    // Set the ramdisk in command line arguments
    UnicodeSPrint (
      (CHAR16 *)KernelArgs + StrLen (KernelArgs), BOOTIMG_KERNEL_ARGS_SIZE,
      L" initrd=0x%x,0x%x",
      (UINTN)Ramdisk, (UINTN)RamdiskSize
      );

    // Append platform kernel arguments
    Status = mAbootimg->AppendArgs (KernelArgs, BOOTIMG_KERNEL_ARGS_SIZE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = mAbootimg->UpdateDtb (FdtBase, &NewFdtBase);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Sanity checks on the new FDT blob.
  //
  err = fdt_check_header ((VOID*)(UINTN)NewFdtBase);
  if (err != 0) {
    Print (L"ERROR: Device Tree header not valid (err:%d)\n", err);
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->InstallConfigurationTable (
                  &gFdtTableGuid,
                  (VOID *)(UINTN)NewFdtBase
                  );
  return Status;
}

STATIC
EFI_STATUS
GetPartition (
  IN  CHAR16                          *PathStr,
  OUT EFI_BLOCK_IO_PROTOCOL           **BlockIo
  )
{
  EFI_STATUS                          Status;
  EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL  *EfiDevicePathFromTextProtocol;
  EFI_DEVICE_PATH                     *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL            *Node, *NextNode;
  EFI_HANDLE                          Handle;

  if (PathStr == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Status = gBS->LocateProtocol (&gEfiDevicePathFromTextProtocolGuid, NULL, (VOID **)&EfiDevicePathFromTextProtocol);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  DevicePath = (EFI_DEVICE_PATH *)EfiDevicePathFromTextProtocol->ConvertTextToDevicePath (PathStr);
  if (DevicePath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  /* Find DevicePath node of Partition */
  NextNode = DevicePath;
  while (1) {
    Node = NextNode;
    if (IS_DEVICE_PATH_NODE (Node, MEDIA_DEVICE_PATH, MEDIA_HARDDRIVE_DP)) {
      break;
    }
    NextNode = NextDevicePathNode (Node);
  }

  Status = gBS->LocateDevicePath (&gEfiDevicePathProtocolGuid, &DevicePath, &Handle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->OpenProtocol (
                  Handle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **) BlockIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to get BlockIo: %r\n", Status));
    return Status;
  }

  return Status;
}

STATIC
EFI_STATUS
LoadBootImage (
  IN  CHAR16                         *BootPathStr,
  OUT VOID                           **Buffer,
  OUT UINTN                          *BufferSize
  )
{
  EFI_STATUS                 Status;
  EFI_BLOCK_IO_PROTOCOL      *BlockIo;
  UINTN                      BootImageSize = 0;

  if ((Buffer == NULL) || (BufferSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  // Read boot device to get kernel
  Status = GetPartition (BootPathStr, &BlockIo);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  // Read both image header and kernel header
  *Buffer = AllocatePages (EFI_SIZE_TO_PAGES (BlockIo->Media->BlockSize * BOOTIMG_HEADER_BLOCKS));
  if (Buffer == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }
  Status = BlockIo->ReadBlocks (
                      BlockIo,
                      BlockIo->Media->MediaId,
                      0,
                      BlockIo->Media->BlockSize * BOOTIMG_HEADER_BLOCKS,
                      *Buffer
                      );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Get the real size of boot image
  Status = GetImgSize (*Buffer, &BootImageSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to get Abootimg Size: %r\n", Status));
    return Status;
  }
  FreePages (*Buffer, EFI_SIZE_TO_PAGES (BlockIo->Media->BlockSize * BOOTIMG_HEADER_BLOCKS));
  BootImageSize = ALIGN_VALUE (BootImageSize, BlockIo->Media->BlockSize);

  /* Both PartitionStart and PartitionSize are counted as block size. */
  *Buffer = AllocatePages (EFI_SIZE_TO_PAGES (BootImageSize));
  if (Buffer == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }

  /* Load the full boot.img */
  Status = BlockIo->ReadBlocks (
                      BlockIo,
                      BlockIo->Media->MediaId,
                      0,
                      BootImageSize,
                      *Buffer
                      );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to read blocks: %r\n", Status));
    return Status;
  }
  return Status;
}

/*
 * Boot from RAM
 */
STATIC
EFI_STATUS
BootFromRam (
  IN     VOID                   *Buffer,
  IN     CHAR16                 *BootPathStr,
  IN     CHAR16                 *FdtPathStr,
  IN OUT VOID                   *KernelArgs,
     OUT VOID                   **Kernel,
     OUT UINTN                  *KernelSize
  )
{
  EFI_STATUS                 Status;
  VOID                       *BootImage = NULL;
  VOID                       *CompressedKernel;
  VOID                       *Fdt;
  VOID                       *Ramdisk;
  VOID                       *StoredKernel;
  UINTN                      BootImageSize;
  UINTN                      CompressedKernelSize;
  UINTN                      RamdiskSize;
  UINTN                      StoredKernelSize;

  Status = AbootimgGetKernelInfo (Buffer, Kernel, KernelSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to get kernel information from Android Boot Image: %r\n", Status));
    return Status;
  }
  Status = CheckKernelImageHeader (*Kernel);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "The kernel is not raw format.\n"));
    CompressedKernel = *Kernel;
    CompressedKernelSize = *KernelSize;
    Status = UncompressKernel (
               CompressedKernel,
               &CompressedKernelSize,
               Kernel,
               KernelSize
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to uncompress kernel with gzip format: %r\n", Status));
      return Status;
    }
    // gzip kernel with attached FDT
    Fdt = CompressedKernel + CompressedKernelSize;
    Status = GetAttachedFdt (*Kernel, &Fdt);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to load FDT from gzip kernel\n"));
      if (BootImage == NULL) {
        Status = LoadBootImage (BootPathStr, &BootImage, &BootImageSize);
        if (EFI_ERROR (Status)) {
          DEBUG ((DEBUG_ERROR, "Failed to load boot image from partition: %r\n", Status));
          return Status;
        }
      }
      // Get FDT from the end of kernel that is located in partition
      Status = AbootimgGetKernelInfo (BootImage, &StoredKernel, &StoredKernelSize);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Failed to get kernel information from stored Android Boot Image: %r\n", Status));
        return Status;
      }
      // Get FDT from the end of kernel that is located in partition if it's raw kernel
      Status = CheckKernelImageHeader (StoredKernel);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Stored kernel image is not raw format: %r\n", Status));
        CompressedKernel = StoredKernel;
        CompressedKernelSize = StoredKernelSize;
        Status = UncompressKernel (
                   CompressedKernel,
                   &CompressedKernelSize,
                   &StoredKernel,
                   &StoredKernelSize
                   );
        if (EFI_ERROR (Status)) {
          DEBUG ((DEBUG_ERROR, "Failed to uncompress stored kernel with gzip format: %r\n", Status));
          return Status;
        }
        // Get FDT from the end of stored gzip kernel that is located in partition
        Fdt = CompressedKernel + CompressedKernelSize;
      }
      Status = GetAttachedFdt (StoredKernel, &Fdt);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Failed to get attached FDT from the end of stored kernel: %r\n", Status));
        return Status;
      }
    }
  } else {
    // raw kernel with attached FDT
    // Get FDT from the end of raw kernel that is located in RAM
    Status = GetAttachedFdt (*Kernel, &Fdt);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to get attached FDT from the end of raw kernel: %r\n", Status));
      if (BootImage == NULL) {
        Status = LoadBootImage (BootPathStr, &BootImage, &BootImageSize);
        if (EFI_ERROR (Status)) {
          DEBUG ((DEBUG_ERROR, "Failed to load boot image from partition: %r\n", Status));
          return Status;
        }
      }
      // Get FDT from the end of raw kernel that is located in partition
      Status = AbootimgGetKernelInfo (BootImage, &StoredKernel, &StoredKernelSize);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Failed to get kernel information from stored Android Boot Image: %r\n", Status));
        return Status;
      }
      // Get FDT from the end of raw kernel that is located in partition
      Status = CheckKernelImageHeader (StoredKernel);
      if (EFI_ERROR (Status)) {
          DEBUG ((DEBUG_ERROR, "Wrong kernel image: %r\n", Status));
          return Status;
      }
      Status = GetAttachedFdt (StoredKernel, &Fdt);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Failed to get attached FDT from the end of stored raw kernel: %r\n", Status));
        return Status;
      }
    }
  }
  // Get ramdisk from boot image in RAM
  Status = GetRamdiskInfo (Buffer, &Ramdisk, &RamdiskSize);
  if (RamdiskSize == 0) {
    if (BootImage == NULL) {
      Status = LoadBootImage (BootPathStr, &BootImage, &BootImageSize);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Failed to load boot image from partition: %r\n", Status));
        return Status;
      }
    }
    // Get ramdisk from boot image in partition
    Status = GetRamdiskInfo (BootImage, &Ramdisk, &RamdiskSize);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to get ramdisk from boot image: %r\n", Status));
      return Status;
    }
    Status = InstallFdt (BootImage, (UINTN)Fdt, KernelArgs);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to install FDT: %r\n", Status));
      return Status;
    }
  } else {
    Status = InstallFdt (Buffer, (UINTN)Fdt, KernelArgs);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to install FDT: %r\n", Status));
      return Status;
    }
  }
  return EFI_SUCCESS;
}

/*
 * Boot from partition
 */
STATIC
EFI_STATUS
BootFromPartition (
  IN     CHAR16              *BootPathStr,
  IN     CHAR16              *FdtPathStr,
  IN OUT VOID                *KernelArgs,
     OUT VOID                **Kernel,
     OUT UINTN               *KernelSize
  )
{
  EFI_STATUS                 Status;
  VOID                       *BootImage;
  VOID                       *CompressedKernel;
  VOID                       *Fdt;
  VOID                       *Ramdisk;
  UINTN                      BootImageSize;
  UINTN                      CompressedKernelSize;
  UINTN                      RamdiskSize;

  Status = LoadBootImage (BootPathStr, &BootImage, &BootImageSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to load boot image from boot partition: %r\n", Status));
    return Status;
  }
  Status = AbootimgGetKernelInfo (BootImage, Kernel, KernelSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to get kernel information from Android Boot Image: %r\n", Status));
    return Status;
  }
  Status = CheckKernelImageHeader (*Kernel);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "The kernel image is not raw format: %r\n", Status));
    CompressedKernel = *Kernel;
    CompressedKernelSize = *KernelSize;
    Status = UncompressKernel (
               CompressedKernel,
               &CompressedKernelSize,
               Kernel,
               KernelSize
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to uncompress kernel with gzip format: %r\n", Status));
      return Status;
    }
    // gzip kernel with attached FDT
    Fdt = CompressedKernel + CompressedKernelSize;
  }
  // Get FDT from the end of kernel in boot image
  Status = GetAttachedFdt (*Kernel, &Fdt);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to get attached FDT from the end of raw kernel: %r\n", Status));
    return Status;
  }
  // Get ramdisk from boot image
  Status = GetRamdiskInfo (BootImage, &Ramdisk, &RamdiskSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to get ramdisk from boot image: %r\n", Status));
    return Status;
  }
  Status = InstallFdt (BootImage, (UINTN)Fdt, KernelArgs);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to install FDT: %r\n", Status));
    return Status;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
AbootimgBootRam (
  IN VOID                            *Buffer,
  IN UINTN                            BufferSize,
  IN CHAR16                          *BootPathStr,
  IN CHAR16                          *FdtPathStr
  )
{
  EFI_STATUS                          Status;
  VOID                               *Kernel;
  UINTN                               KernelSize;
  MEMORY_DEVICE_PATH                  KernelDevicePath;
  EFI_HANDLE                          ImageHandle;
  VOID                               *NewKernelArgs;
  EFI_LOADED_IMAGE_PROTOCOL          *ImageInfo;

  NewKernelArgs = AllocateZeroPool (BOOTIMG_KERNEL_ARGS_SIZE << 1);
  if (NewKernelArgs == NULL) {
    DEBUG ((DEBUG_ERROR, "Fail to allocate memory\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BootFromRam (Buffer, BootPathStr, FdtPathStr, NewKernelArgs, &Kernel, &KernelSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to boot from RAM: %r\n", Status));
    return Status;
  }

  CopyMem (&KernelDevicePath, &MemoryDevicePathTemplate, sizeof (MemoryDevicePathTemplate));

  // Have to cast to UINTN before casting to EFI_PHYSICAL_ADDRESS in order to
  // appease GCC.
  KernelDevicePath.Node1.StartingAddress = (EFI_PHYSICAL_ADDRESS)(UINTN) Kernel;
  KernelDevicePath.Node1.EndingAddress   = (EFI_PHYSICAL_ADDRESS)(UINTN) Kernel + KernelSize;

  Status = gBS->LoadImage (TRUE, gImageHandle, (EFI_DEVICE_PATH *)&KernelDevicePath, (VOID*)(UINTN)Kernel, KernelSize, &ImageHandle);

  // Set kernel arguments
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **) &ImageInfo);
  ImageInfo->LoadOptions = NewKernelArgs;
  ImageInfo->LoadOptionsSize = StrLen (NewKernelArgs) * sizeof (CHAR16);

  // Before calling the image, enable the Watchdog Timer for  the 5 Minute period
  gBS->SetWatchdogTimer (5 * 60, 0x0000, 0x00, NULL);
  // Start the image
  Status = gBS->StartImage (ImageHandle, NULL, NULL);
  // Clear the Watchdog Timer after the image returns
  gBS->SetWatchdogTimer (0x0000, 0x0000, 0x0000, NULL);
  return EFI_SUCCESS;
}

EFI_STATUS
AbootimgBootPartition (
  IN CHAR16                          *BootPathStr,
  IN CHAR16                          *FdtPathStr
  )
{
  EFI_STATUS                          Status;
  VOID                               *Kernel;
  UINTN                               KernelSize;
  MEMORY_DEVICE_PATH                  KernelDevicePath;
  EFI_HANDLE                          ImageHandle;
  VOID                               *NewKernelArgs;
  EFI_LOADED_IMAGE_PROTOCOL          *ImageInfo;

  NewKernelArgs = AllocateZeroPool (BOOTIMG_KERNEL_ARGS_SIZE << 1);
  if (NewKernelArgs == NULL) {
    DEBUG ((DEBUG_ERROR, "Fail to allocate memory\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BootFromPartition (BootPathStr, FdtPathStr, NewKernelArgs, &Kernel, &KernelSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to boot from partition: %r\n", Status));
    return Status;
  }

  CopyMem (&KernelDevicePath, &MemoryDevicePathTemplate, sizeof (MemoryDevicePathTemplate));

  // Have to cast to UINTN before casting to EFI_PHYSICAL_ADDRESS in order to
  // appease GCC.
  KernelDevicePath.Node1.StartingAddress = (EFI_PHYSICAL_ADDRESS)(UINTN) Kernel;
  KernelDevicePath.Node1.EndingAddress   = (EFI_PHYSICAL_ADDRESS)(UINTN) Kernel + KernelSize;

  Status = gBS->LoadImage (TRUE, gImageHandle, (EFI_DEVICE_PATH *)&KernelDevicePath, (VOID*)(UINTN)Kernel, KernelSize, &ImageHandle);

  // Set kernel arguments
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **) &ImageInfo);
  ImageInfo->LoadOptions = NewKernelArgs;
  ImageInfo->LoadOptionsSize = StrLen (NewKernelArgs) * sizeof (CHAR16);

  // Before calling the image, enable the Watchdog Timer for  the 5 Minute period
  gBS->SetWatchdogTimer (5 * 60, 0x0000, 0x00, NULL);
  // Start the image
  Status = gBS->StartImage (ImageHandle, NULL, NULL);
  // Clear the Watchdog Timer after the image returns
  gBS->SetWatchdogTimer (0x0000, 0x0000, 0x0000, NULL);
  return EFI_SUCCESS;
}