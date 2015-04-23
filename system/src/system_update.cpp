
#include <stddef.h>
#include "ota_flash_hal.h"
#include "core_hal.h"
#include "delay_hal.h"
#include "spark_wiring_stream.h"
#include "spark_wiring_rgb.h"
#include "system_update.h"
#include "system_cloud.h"
#include "rgbled.h"
#include "module_info.h"
#include "spark_wiring_usbserial.h"
#include "system_ymodem.h"
#include "system_task.h"
#include "spark_wiring_system.h"
#include "spark_protocol_functions.h"
#include "hw_config.h"
#include "string_convert.h"
#include "appender.h"

#ifdef START_DFU_FLASHER_SERIAL_SPEED
static uint32_t start_dfu_flasher_serial_speed = START_DFU_FLASHER_SERIAL_SPEED;
#endif
#ifdef START_YMODEM_FLASHER_SERIAL_SPEED
static uint32_t start_ymodem_flasher_serial_speed = START_YMODEM_FLASHER_SERIAL_SPEED;
#endif

ymodem_serial_flash_update_handler Ymodem_Serial_Flash_Update_Handler = NULL;

volatile uint8_t SPARK_CLOUD_CONNECT = 1; //default is AUTOMATIC mode
volatile uint8_t SPARK_CLOUD_SOCKETED;
volatile uint8_t SPARK_CLOUD_CONNECTED;
volatile uint8_t SPARK_FLASH_UPDATE;
volatile uint32_t TimingFlashUpdateTimeout;

void set_ymodem_serial_flash_update_handler(ymodem_serial_flash_update_handler handler)
{
    Ymodem_Serial_Flash_Update_Handler = handler;
}

void set_start_dfu_flasher_serial_speed(uint32_t speed)
{
#ifdef START_DFU_FLASHER_SERIAL_SPEED
    start_dfu_flasher_serial_speed = speed;
#endif
}

void set_start_ymodem_flasher_serial_speed(uint32_t speed)
{
#ifdef START_YMODEM_FLASHER_SERIAL_SPEED
    start_ymodem_flasher_serial_speed = speed;
#endif
}

bool system_serialFirmwareUpdate(Stream* stream) 
{
#if PLATFORM_ID>2    
    set_ymodem_serial_flash_update_handler(Ymodem_Serial_Flash_Update);
#endif    
    FileTransfer::Descriptor desc;    
    desc.chunk_size = 0;    
    desc.file_address = HAL_OTA_FlashAddress();
    desc.file_length = HAL_OTA_FlashLength();
    desc.store = FileTransfer::Store::FIRMWARE;
    return system_serialFileTransfer(stream, desc);
}


bool system_serialFileTransfer(Stream *serialObj, FileTransfer::Descriptor& file)
{
    bool status = false;
    
    if (NULL != Ymodem_Serial_Flash_Update_Handler)
    {        
        status = Ymodem_Serial_Flash_Update_Handler(serialObj, file, NULL);
        SPARK_FLASH_UPDATE = 0;
        TimingFlashUpdateTimeout = 0;

        if (status == true)
        {
            if (file.store==FileTransfer::Store::FIRMWARE) {
                serialObj->println("Restarting system to apply firmware update...");
                HAL_Delay_Milliseconds(100);
                HAL_Core_System_Reset();
            }
        }
    }
    else
    {
        serialObj->println("Firmware update using this terminal is not supported!");
        serialObj->println("Add #include \"Ymodem/Ymodem.h\" to your sketch and try again.");
    }
    return status;
}

void system_lineCodingBitRateHandler(uint32_t bitrate)
{
#ifdef START_DFU_FLASHER_SERIAL_SPEED
    if (bitrate == start_dfu_flasher_serial_speed)
    {
        //Reset device and briefly enter DFU bootloader mode
        System.dfu(false);
    }
#endif
#ifdef START_YMODEM_FLASHER_SERIAL_SPEED
    if (!WLAN_SMART_CONFIG_START && bitrate == start_ymodem_flasher_serial_speed)
    {
        //Set the Ymodem flasher flag to execute system_serialFirmwareUpdate()
        set_ymodem_serial_flash_update_handler(Ymodem_Serial_Flash_Update);
        RGB.control(true);
        RGB.color(RGB_COLOR_MAGENTA);
        SPARK_FLASH_UPDATE = 3;
        TimingFlashUpdateTimeout = 0;
    }
#endif
}

int Spark_Prepare_For_Firmware_Update(FileTransfer::Descriptor& file, uint32_t flags, void* reserved)
{    
    if (file.store==FileTransfer::Store::FIRMWARE) 
    {
        // address is relative to the OTA region. Normally will be 0.
        file.file_address = HAL_OTA_FlashAddress() + file.chunk_address;
        
        // chunk_size 0 indicates defaults.
        if (file.chunk_size==0) {
            file.chunk_size = HAL_OTA_ChunkSize();
            file.file_length = HAL_OTA_FlashLength();
        }        
    }
    int result = 0;
    if (flags & 1) {
        // only check address
    }
    else {
        RGB.control(true);
        RGB.color(RGB_COLOR_MAGENTA);
        SPARK_FLASH_UPDATE = 1;
        TimingFlashUpdateTimeout = 0;
        HAL_FLASH_Begin(file.file_address, file.file_length);
    }
    return result;
}

#ifdef MODULAR_FIRMWARE
#define USER_OTA_MODULE_FUNCTION    MODULE_FUNCTION_USER_PART
#else
#define USER_OTA_MODULE_FUNCTION    MODULE_FUNCTION_MONO_FIRMWARE
#endif

void serial_dump(const char* msg, ...);

int Spark_Finish_Firmware_Update(FileTransfer::Descriptor& file, uint32_t flags, void* reserved)
{
    SPARK_FLASH_UPDATE = 0;
    TimingFlashUpdateTimeout = 0;
    //serial_dump("update finished flags=%d store=%d", flags, file.store);
    
    if (flags & 1) {    // update successful
        if (file.store==FileTransfer::Store::FIRMWARE)
        {
#ifdef FLASH_UPDATE_MODULES
            // todo - VerifyCRC should also take a device (FLASH_INTERNAL | FLASH_EXTERNAL)
            // and must verify that the address/length is within range
            uint32_t ota_address = HAL_OTA_FlashAddress();
            uint32_t moduleAddress = HAL_FLASH_ModuleAddress(ota_address);            
            uint32_t moduleLength = HAL_FLASH_ModuleLength(ota_address);
            
            if (FLASH_CheckValidAddressRange(FLASH_INTERNAL, moduleAddress, moduleLength + 4) &&
                HAL_FLASH_VerifyCRC32(ota_address, moduleLength))
            {
                HAL_FLASH_AddToNextAvailableModulesSlot(FLASH_INTERNAL, ota_address,
                                                        FLASH_INTERNAL, moduleAddress,
                                                        (moduleLength + 4),//+4 to copy the CRC too                
                                                        USER_OTA_MODULE_FUNCTION,
                                                        MODULE_VERIFY_CRC|MODULE_VERIFY_DESTINATION_IS_START_ADDRESS|MODULE_VERIFY_FUNCTION);//true to verify the CRC during copy also

                HAL_FLASH_End();

            }
#else
            HAL_FLASH_End();
#endif
            //serial_dump("resetting");            
            // todo - talk with application and see if now is a good time to reset
            // if update not applied, do we need to reset?
            HAL_Core_System_Reset();        
        }
    }
    RGB.control(false);
    return 0;
}

int Spark_Save_Firmware_Chunk(FileTransfer::Descriptor& file, const uint8_t* chunk, void* reserved)
{    
    TimingFlashUpdateTimeout = 0;
    int result = -1;
    if (file.store==FileTransfer::Store::FIRMWARE)
    {
        result = HAL_FLASH_Update(chunk, file.chunk_address, file.chunk_size);
        LED_Toggle(LED_RGB);
    }
    return result;
}

class AppendJson
{
    appender_fn fn;
    void* data;
    
public:

    AppendJson(appender_fn fn, void* data) {
        this->fn = fn; this->data = data;
    }
    
    bool write_quoted(const char* value) {
        return write('"') &&
               write(value) &&
               write('"');
    }
    
    bool write_attribute(const char* name) {
        return 
                write_quoted(name) &&
                write(':');
    }
    
    bool write_string(const char* name, const char* value) {
        return write_attribute(name) &&
               write_quoted(value) &&
               next();
    }
    
    bool newline() { return write("\r\n"); }
    
    bool write_value(const char* name, int value) {
        char buf[10];
        itoa(value, buf, 10);
        return write_attribute(name) &&
               write(buf) &&
               next();
    }
    
    bool end_list() {
        return write_attribute("_") &&
               write_quoted("");
    }
    
    bool write(char c) {
        return fn(data, (const uint8_t*)&c, 1);
    }
    
    bool write(const char* string) {
        return fn(data, (const uint8_t*)string, strlen(string));
    }
    
    bool next() { return write(',') && newline(); }
};

const char* module_function_string(module_function_t func) {
    switch (func) {
        case MODULE_FUNCTION_NONE: return "none";
        case MODULE_FUNCTION_RESOURCE: return "res";
        case MODULE_FUNCTION_BOOTLOADER: return "boot";
        case MODULE_FUNCTION_MONO_FIRMWARE: return "mono";
        case MODULE_FUNCTION_SYSTEM_PART: return "system";
        case MODULE_FUNCTION_USER_PART: return "user";
        default: return "unknown";
    }    
}

const char* module_name(uint8_t index, char* buf)
{
    return itoa(index, buf, 10);
}

bool system_info_to_json(appender_fn append, void* append_data, hal_system_info_t& system)
{
    AppendJson json(append, append_data);
    bool result = true;
    result &= json.write_value("p", system.platform_id)  
        && json.write_attribute("m")
        && json.write('[');
    char buf[65];
    for (unsigned i=0; i<system.module_count; i++) {        
        if (i) result &= json.write(',');
        const hal_module_t& module = system.modules[i];
        const module_info_t* info = module.info;
        buf[64] = 0;
        result &= json.write('{') && json.write_value("s", module.bounds.maximum_size)
          && json.write_string("u", bytes2hexbuf(module.suffix->sha, 32, buf))
          && json.write_string("f", module_function_string(module_function(info)))
          && json.write_string("n", module_name(module_index(info), buf))
          && json.write_value("v", info->module_version)
        // on the photon we have just one dependency, this will need generalizing for other platforms
          && json.write_attribute("d")
          && json.write('[');
          for (unsigned int d=0; d<1; d++) {
              const module_dependency_t& dependency = info->dependency;
              if (d) result &= json.write(',');
              result &= json.write('{') 
                && json.write_string("f", module_function_string(module_function_t(dependency.module_function)))
                && json.write_string("n", module_name(dependency.module_index, buf))
                && json.write_value("v", dependency.module_version)
                 && json.end_list() && json.write('}');
          }          
          result &= json.write("]}");
    }
    
    result &= json.write(']');
    return result;
}


bool system_module_info(appender_fn append, void* append_data, void* reserved)
{
    hal_system_info_t info;    
    HAL_System_Info(&info, true, NULL);
    bool result = system_info_to_json(append, append_data, info);
    HAL_System_Info(&info, false, NULL);
    return result;
}

