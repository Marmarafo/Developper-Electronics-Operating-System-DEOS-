// DEOS -- IOMMU/VT-d tespiti (DMA remapping yapisi var mi?)
#ifndef DEOS_IOMMU_H
#define DEOS_IOMMU_H
#include "types.h"

int iommu_detect(void);   // 1 = VT-d DMA remapping yapisi mevcut
const char *iommu_name(void);

#endif
