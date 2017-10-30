//
// PROJECT:         Aspia Remote Desktop
// FILE:            base/devices/smbios.cc
// LICENSE:         Mozilla Public License Version 2.0
// PROGRAMMERS:     Dmitry Chapyshev (dmitry@aspia.ru)
//

#include "base/devices/smbios.h"
#include "base/strings/string_util.h"
#include "base/logging.h"

namespace aspia {

// static
std::unique_ptr<SMBios> SMBios::Create(std::unique_ptr<uint8_t[]> data, size_t data_size)
{
    if (!data || !data_size)
        return nullptr;

    SMBiosData* smbios = reinterpret_cast<SMBiosData*>(data.get());

    int table_count = GetTableCount(smbios->smbios_table_data, smbios->length);
    if (!table_count)
    {
        LOG(WARNING) << "SMBios tables not found";
        return nullptr;
    }

    return std::unique_ptr<SMBios>(new SMBios(std::move(data), data_size, table_count));
}

SMBios::SMBios(std::unique_ptr<uint8_t[]> data, size_t data_size, int table_count)
    : data_(std::move(data)),
      data_size_(data_size),
      table_count_(table_count)
{
    DCHECK(data_ != nullptr);
    DCHECK(data_size_ != 0);
    DCHECK(table_count_ != 0);
}

uint8_t SMBios::GetMajorVersion() const
{
    SMBiosData* smbios = reinterpret_cast<SMBiosData*>(data_.get());
    return smbios->smbios_major_version;
}

uint8_t SMBios::GetMinorVersion() const
{
    SMBiosData* smbios = reinterpret_cast<SMBiosData*>(data_.get());
    return smbios->smbios_minor_version;
}

// static
int SMBios::GetTableCount(const uint8_t* table_data, uint32_t length)
{
    int count = 0;

    const uint8_t* pos = table_data;
    const uint8_t* end = table_data + length;

    while (pos < end)
    {
        // Increase the position by the length of the structure.
        pos += pos[1];

        // Increses the offset to point to the next header that's after the strings at the end
        // of the structure.
        while (*reinterpret_cast<const uint16_t*>(pos) != 0 && pos < end)
            ++pos;

        // Points to the next stucture thas after two null bytes at the end of the strings.
        pos += 2;

        ++count;
    }

    return count;
}

const uint8_t* SMBios::GetTable(TableType type) const
{
    SMBiosData* smbios = reinterpret_cast<SMBiosData*>(data_.get());

    const uint8_t* start = &smbios->smbios_table_data[0];
    const uint8_t* end = start + smbios->length;
    const uint8_t* pos = start;

    int table_index = 0;

    while (table_index < table_count_ && pos + 4 <= end)
    {
        const uint8_t table_type = pos[0];
        const uint8_t table_length = pos[1];

        if (table_length < 4)
        {
            // If a short entry is found(less than 4 bytes), not only it is invalid, but we
            // cannot reliably locate the next entry. Better stop at this point, and let the
            // user know his / her table is broken.
            LOG(WARNING) << "Invalid SMBIOS table length: " << table_length;
            break;
        }

        // The table of the specified type is found.
        if (table_type == type)
            return pos;

        if (table_type == TABLE_TYPE_END_OF_TABLE)
            break;

        // Look for the next handle.
        const uint8_t* next = pos + table_length;
        while (static_cast<uintptr_t>(next - start + 1) < smbios->length &&
               (next[0] != 0 || next[1] != 0))
        {
            ++next;
        }

        // Points to the next table thas after two null bytes at the end of the strings.
        pos = next + 2;

        ++table_index;
    }

    if (table_index + 1 != table_count_)
    {
        LOG(WARNING) << "The number of processed tables does not correspond "
                        "to the total number of tables: "
                     << (table_index + 1) << "/" << table_count_;
    }

    if (static_cast<uintptr_t>(pos - start) != smbios->length)
    {
        LOG(WARNING) << "The announced size does not match the processed: "
                     << static_cast<uintptr_t>(pos - start) << "/" << smbios->length;
    }

    // Table not found.
    return nullptr;
}

//
// SMBiosTable
//

SMBios::Table::Table(const SMBios& smbios, TableType type)
    : data_(smbios.GetTable(type))
{
    // Nothing
}

bool SMBios::Table::IsValid() const
{
    return data_ != nullptr;
}

uint8_t SMBios::Table::GetByte(uint8_t offset) const
{
    return data_[offset];
}

uint16_t SMBios::Table::GetWord(uint8_t offset) const
{
    return *reinterpret_cast<const uint16_t*>(GetPointer(offset));
}

uint32_t SMBios::Table::GetDword(uint8_t offset) const
{
    return *reinterpret_cast<const uint32_t*>(GetPointer(offset));
}

uint64_t SMBios::Table::GetQword(uint8_t offset) const
{
    return *reinterpret_cast<const uint64_t*>(GetPointer(offset));
}

std::string SMBios::Table::GetString(uint8_t offset) const
{
    uint8_t handle = GetByte(offset);
    if (!handle)
        return std::string();

    char* string = reinterpret_cast<char*>(const_cast<uint8_t*>(data_)) + data_[1];

    while (handle > 1 && *string)
    {
        string += strlen(string);
        ++string;
        --handle;
    }

    if (!*string || !string[0])
        return std::string();

    return string;
}

const uint8_t* SMBios::Table::GetPointer(uint8_t offset) const
{
    return &data_[offset];
}

//
// BiosTable
//

SMBios::BiosTable::BiosTable(const SMBios& smbios)
    : Table(smbios, TABLE_TYPE_BIOS)
{
    // Nothing
}

std::string SMBios::BiosTable::GetManufacturer() const
{
    return GetString(0x04);
}

std::string SMBios::BiosTable::GetVersion() const
{
    return GetString(0x05);
}

std::string SMBios::BiosTable::GetDate() const
{
    return GetString(0x08);
}

int SMBios::BiosTable::GetSize() const
{
    return (GetByte(0x09) + 1) << 6;
}

std::string SMBios::BiosTable::GetBiosRevision() const
{
    uint8_t major = GetByte(0x14);
    uint8_t minor = GetByte(0x15);

    if (major != 0xFF && minor != 0xFF)
        return StringPrintf("%u.%u", major, minor);

    return std::string();
}

std::string SMBios::BiosTable::GetFirmwareRevision() const
{
    uint8_t major = GetByte(0x16);
    uint8_t minor = GetByte(0x17);

    if (major != 0xFF && minor != 0xFF)
        return StringPrintf("%u.%u", major, minor);

    return std::string();
}

std::string SMBios::BiosTable::GetAddress() const
{
    uint16_t address = GetWord(0x06);

    if (address != 0)
        return StringPrintf("%04X0h", address);

    return std::string();
}

int SMBios::BiosTable::GetRuntimeSize() const
{
    uint16_t address = GetWord(0x06);
    if (address == 0)
        return 0;

    uint32_t code = (0x10000 - address) << 4;

    if (code & 0x000003FF)
        return code;

    return (code >> 10) * 1024;
}

SMBios::BiosTable::FeatureList SMBios::BiosTable::GetCharacteristics() const
{
    FeatureList feature_list;

    static const char* characteristics_names[] =
    {
        "BIOS characteristics not supported", // 3
        "ISA", // 4
        "MCA",
        "EISA",
        "PCI",
        "PC Card (PCMCIA)",
        "PNP",
        "APM",
        "BIOS is upgradeable",
        "BIOS shadowing",
        "VLB",
        "ESCD",
        "Boot from CD",
        "Selectable boot",
        "BIOS ROM is socketed",
        "Boot from PC Card (PCMCIA)",
        "EDD",
        "Japanese floppy for NEC 9800 1.2 MB (int 13h)",
        "Japanese floppy for Toshiba 1.2 MB (int 13h)",
        "5.25\"/360 kB floppy (int 13h)",
        "5.25\"/1.2 MB floppy (int 13h)",
        "3.5\"/720 kB floppy (int 13h)",
        "3.5\"/2.88 MB floppy (int 13h)",
        "Print screen (int 5h)",
        "8042 keyboard (int 9h)",
        "Serial (int 14h)",
        "Printer (int 17h)",
        "CGA/mono video (int 10h)",
        "NEC PC-98" // 31
    };

    uint64_t characteristics = GetQword(0x0A);
    if (!(characteristics & (1 << 3)))
    {
        for (int i = 4; i <= 31; ++i)
        {
            bool is_supported = (characteristics & (1ULL << i)) ? true : false;
            feature_list.emplace_back(characteristics_names[i - 3], is_supported);
        }
    }

    uint8_t table_length = GetByte(0x01);

    if (table_length >= 0x13)
    {
        uint8_t characteristics1 = GetByte(0x12);

        static const char* characteristics1_names[] =
        {
            "ACPI", // 0
            "USB legacy",
            "AGP",
            "I2O boot",
            "LS-120 boot",
            "ATAPI Zip drive boot",
            "IEEE 1394 boot",
            "Smart battery" // 7
        };

        for (int i = 0; i <= 7; ++i)
        {
            bool is_supported = (characteristics1 & (1 << i)) ? true : false;
            feature_list.emplace_back(characteristics1_names[i], is_supported);
        }
    }

    if (table_length >= 0x14)
    {
        uint8_t characteristics2 = GetByte(0x13);

        static const char* characteristics2_names[] =
        {
            "BIOS boot specification", // 0
            "Function key-initiated network boot",
            "Targeted content distribution" // 2
        };

        for (int i = 0; i <= 2; ++i)
        {
            bool is_supported = (characteristics2 & (1 << i)) ? true : false;
            feature_list.emplace_back(characteristics2_names[i], is_supported);
        }
    }

    return feature_list;
}

//
// SystemTable
//

SMBios::SystemTable::SystemTable(const SMBios& smbios)
    : Table(smbios, TABLE_TYPE_SYSTEM),
      major_version_(smbios.GetMajorVersion()),
      minor_version_(smbios.GetMinorVersion())
{
    // Nothing
}

std::string SMBios::SystemTable::GetManufacturer() const
{
    return GetString(0x04);
}

std::string SMBios::SystemTable::GetProductName() const
{
    return GetString(0x05);
}

std::string SMBios::SystemTable::GetVersion() const
{
    return GetString(0x06);
}

std::string SMBios::SystemTable::GetSerialNumber() const
{
    return GetString(0x07);
}

std::string SMBios::SystemTable::GetUUID() const
{
    uint8_t table_length = GetByte(0x01);
    if (table_length < 0x19)
        return std::string();

    const uint8_t* ptr = GetPointer(0x08);

    bool only_0xFF = true;
    bool only_0x00 = true;

    for (int i = 0; i < 16 && (only_0x00 || only_0xFF); ++i)
    {
        if (ptr[i] != 0x00) only_0x00 = false;
        if (ptr[i] != 0xFF) only_0xFF = false;
    }

    if (only_0xFF || only_0x00)
        return std::string();

    if ((major_version_ << 8) + minor_version_ >= 0x0206)
    {
        return StringPrintf("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                     ptr[3], ptr[2], ptr[1], ptr[0], ptr[5], ptr[4], ptr[7], ptr[6],
                     ptr[8], ptr[9], ptr[10], ptr[11], ptr[12], ptr[13], ptr[14], ptr[15]);
    }

    return StringPrintf("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                        ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7],
                        ptr[8], ptr[9], ptr[10], ptr[11], ptr[12], ptr[13], ptr[14], ptr[15]);
}

std::string SMBios::SystemTable::GetWakeupType() const
{
    uint8_t table_length = GetByte(0x01);
    if (table_length < 0x19)
        return std::string();

    switch (GetByte(0x18))
    {
        case 0x01:
            return "Other";

        case 0x02:
            return "Unknown";

        case 0x03:
            return "APM Timer";

        case 0x04:
            return "Modem Ring";

        case 0x05:
            return "LAN Remote";

        case 0x06:
            return "Power Switch";

        case 0x07:
            return "PCI PME#";

        case 0x08:
            return "AC Power Restored";

        default:
            return std::string();
    }
}

std::string SMBios::SystemTable::GetSKUNumber() const
{
    uint8_t table_length = GetByte(0x01);
    if (table_length < 0x1B)
        return std::string();

    return GetString(0x19);
}

std::string SMBios::SystemTable::GetFamily() const
{
    uint8_t table_length = GetByte(0x01);
    if (table_length < 0x1B)
        return std::string();

    return GetString(0x1A);
}

//
// BaseboardTable
//

SMBios::BaseboardTable::BaseboardTable(const SMBios& smbios)
    : Table(smbios, TABLE_TYPE_BASEBOARD)
{
    // Nothing
}

std::string SMBios::BaseboardTable::GetManufacturer() const
{
    return GetString(0x04);
}

std::string SMBios::BaseboardTable::GetProductName() const
{
    return GetString(0x05);
}

std::string SMBios::BaseboardTable::GetVersion() const
{
    return GetString(0x06);
}

std::string SMBios::BaseboardTable::GetSerialNumber() const
{
    return GetString(0x07);
}

std::string SMBios::BaseboardTable::GetAssetTag() const
{
    uint8_t table_length = GetByte(0x01);
    if (table_length < 0x09)
        return std::string();

    return GetString(0x08);
}

SMBios::BaseboardTable::FeatureList SMBios::BaseboardTable::GetFeatures() const
{
    uint8_t table_length = GetByte(0x01);
    if (table_length < 0x0A)
        return FeatureList();

    uint8_t features = GetByte(0x09);
    if ((features & 0x1F) == 0)
        return FeatureList();

    static const char* feature_names[] =
    {
        "Board is a hosting board", // 0
        "Board requires at least one daughter board",
        "Board is removable",
        "Board is replaceable",
        "Board is hot swappable" // 4
    };

    FeatureList list;

    for (int i = 0; i <= 4; ++i)
    {
        bool is_supported = (features & (1 << i)) ? true : false;
        list.emplace_back(feature_names[i], is_supported);
    }

    return list;
}

std::string SMBios::BaseboardTable::GetLocationInChassis() const
{
    uint8_t table_length = GetByte(0x01);
    if (table_length < 0x0E)
        return std::string();

    return GetString(0x0A);
}

std::string SMBios::BaseboardTable::GetBoardType() const
{
    uint8_t table_length = GetByte(0x01);
    if (table_length < 0x0E)
        return std::string();

    static const char* type_names[] =
    {
        "Unknown", // 0x01
        "Other",
        "Server Blade",
        "Connectivity Switch",
        "System Management Module",
        "Processor Module",
        "I/O Module",
        "Memory Module",
        "Daughter Board",
        "Motherboard",
        "Processor+Memory Module",
        "Processor+I/O Module",
        "Interconnect Board" // 0x0D
    };

    uint8_t type = GetByte(0x0D);
    if (!type || type > 0x0D)
        return std::string();

    return type_names[type - 1];
}

} // namespace aspia