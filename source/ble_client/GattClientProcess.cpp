#include "GattClientProcess.h"
#include "mbed_rtc_time.h"
#include <ctime>

#define printf(...)  SEGGER_RTT_printf(0,__VA_ARGS__)


void GattClientProcess::init(BLE &ble_interface, events::EventQueue &event_queue)
{
    _ble_interface = &ble_interface;

    _event_queue = &event_queue;
    
    _gattClient = &_ble_interface->gattClient();
 
    printf("Init BLE Gatt Client finished.\n");
        
    _gattClient->onServiceDiscoveryTermination(as_cb(&Self::when_service_discovery_ends));

    _gattClient->onDataRead(as_cb(&Self::when_char_data_read));
}

void GattClientProcess::on_read_sensor_value_complete_cb(mbed::Callback<void(Device_t&)> cb){
    if(_count_cb < CALLBACK_NUM_MAX){
        _post_read_sensor_value_complete_cb[_count_cb] = cb;
        _count_cb++;
    }
}

void GattClientProcess::start_service_discovery(const ble::ConnectionCompleteEvent& event)
{
    // Create a new device structure and store address and connection handle.
    Device_t new_device = { };
    new_device.connection_handle = event.getConnectionHandle();
    // copy 6 byte address
    memcpy(new_device.address,event.getPeerAddress().data(),ADDR_LEN);   
    new_device.address[ADDR_LEN+1] = '\0';
    
    devices.insert(make_pair(new_device.connection_handle,new_device));
    printf("Get connection handle %d Discovering service...",new_device.connection_handle);

    // launch service discovery  peer
    ble_error_t error = _gattClient->launchServiceDiscovery(
        new_device.connection_handle,
        NULL,//as_cb(&Self::when_service_discovered),
        as_cb(&Self::when_characteristic_discovered)
    );

    if(error){
        printf("Error %s\r\n",BLE::errorToString(error));
        _event_queue->call(this,&Self::stop_service_discovery);
        return;
    }
        
    printf("OK\n");

}

void GattClientProcess::when_service_discovery_ends(const ble::connection_handle_t connection_handle)
{
    ble_error_t error;
    printf("Connection hande %d. All services and characteristics discovered, process them.\r\n",connection_handle);
    
    Device_t &dev = devices[connection_handle];
    
    print_device_info(dev);

    if(dev.is_CTC){
        // if peer is a CTS device, read its time value
        printf("read time from handle... %d\n",dev.chars.time_value_handle);
        error = _gattClient->read(
            connection_handle,  // connection handle
            dev.chars.time_value_handle,    // attribute handle
            0);            //offset

        if(error){
            printf("Error %s during read write ble attributes.\n",BLE::errorToString(error));
            _event_queue->call(this,&Self::stop_service_discovery); 
        }

    }else if(dev.is_beacon){

        // send command to beacons and periodiclly requirest measurement value;
        _event_queue->call<Self,void,Device_t&>(this,&Self::send_command_to_beacon,dev);
        // read first characteristic
        _event_queue->call_in<Self,void,ble::connection_handle_t,GattAttribute::Handle_t>(
            PERIOD_READ_BEACON_DATA,this,&Self::read_data_from_handle,dev.connection_handle,dev.chars.sensor_chars[0].data_handle);
    }

}

void GattClientProcess::send_command_to_beacon(Device_t &dev){
 
        ble_error_t error;
        for(int i = 0; i < dev.num_of_chars ;i++){
            for( size_t j = 0 ; j < sizeof(sensor_cmd_list)/sizeof(Sensor_cmd_t);j++){
                if(dev.chars.sensor_chars[i].type == sensor_cmd_list[j].type){
                       error =_gattClient->write(GattClient::GATT_OP_WRITE_CMD,
                        dev.connection_handle,
                        dev.chars.sensor_chars[i].config_handle,
                        sensor_cmd_list[j].len,sensor_cmd_list[j].cmd);
                        printf("send command to light config handle %d\n",
                                dev.chars.sensor_chars[i].config_handle);
                }
            }

         }
            if(error){
                printf("Error %s during read write ble attributes.\n",BLE::errorToString(error));
                _event_queue->call(this,&Self::stop_service_discovery); 
            }
}

void GattClientProcess::read_data_from_handle(ble::connection_handle_t connection_handle, GattAttribute::Handle_t data_handle){
        printf("read data from characteristic data handle %d",data_handle);
        ble_error_t error = _gattClient->read(
            connection_handle,        // connection handle
            data_handle,              // attribute handle
            0); // offset

        if(error){
            printf("Error %s during read ble attributes. connection_handle %d, data handle %d.\n",
            BLE::errorToString(error),
            connection_handle,
            data_handle);
            _event_queue->call(this,&Self::stop_service_discovery); 
        }
}

void GattClientProcess::read_value_all_sensors(){

    for(std::map<ble::connection_handle_t,Device_t>::iterator it = devices.begin(); it != devices.end() ;it++){
        printf("read sensor data from sensor ");
        print_addr(it->second.address);
        printf("\n");
        
            // read first characteristic
            ble_error_t error = _gattClient->read(
            it->second.connection_handle,      // connection handle
            it->second.chars.sensor_chars[0].data_handle,     // attribute handle
            0);                         // offset

            if(error){
                printf("Error %s during read write ble attributes %d.\n",BLE::errorToString(error),
                    it->second.chars.sensor_chars[0].data_handle);
                _event_queue->call(this,&Self::stop_service_discovery); 
            }
        
    }
}

void GattClientProcess::when_characteristic_discovered(const DiscoveredCharacteristic* discovered_characteristic)
{
    // uint16_t uuid = *((const uint16_t*)discovered_characteristic->getUUID().getBaseUUID());
    ble::connection_handle_t connHandle = discovered_characteristic->getConnectionHandle();
    UUID uuid = discovered_characteristic->getUUID();
    Device_t &dev = devices[connHandle];

    // check if it is a CTS device
    if(uuid == UUID_CURRENT_TIME_CHAR){
        dev.is_CTC = true;
        dev.chars.time_value_handle = discovered_characteristic->getValueHandle();      
        return;
    }

    // check sensor uuid list
    for(size_t i = 0; i < sizeof(sensor_char_uuid_list)/sizeof(Sensor_char_uuid_t) ;i++){
        
        if(uuid == sensor_char_uuid_list[i].uuid_config){
           printf("found uuid in list %d\n",i);
            // if it is a target config characteristic, check uuid list and get a sensor type
            dev.is_beacon = true;

            for(size_t j = 0; j < dev.num_of_chars ;j++){
                // if sensor type exist, store config handle of this sensor type
                 if (dev.chars.sensor_chars[j].type == sensor_char_uuid_list[i].type){
                    dev.chars.sensor_chars[j].config_handle = discovered_characteristic->getValueHandle();                    
                    return;      
                }
            }

            // if this is a new sensor type
            dev.chars.sensor_chars[dev.num_of_chars].type = sensor_char_uuid_list[i].type;   
            dev.chars.sensor_chars[dev.num_of_chars].config_handle =  discovered_characteristic->getValueHandle(); 
            dev.num_of_chars++;
            return;
            

        }

        // if it is a target data characteristic, check uuid list and get a sensor type
        if(uuid == sensor_char_uuid_list[i].uuid_data){
            
            dev.is_beacon = true;
            for(int j = 0; j < dev.num_of_chars;j++){
                
                if (dev.chars.sensor_chars[j].type == sensor_char_uuid_list[i].type){
                    // if sensor type exist, store data handle of this sensor type
                    dev.chars.sensor_chars[j].data_handle = discovered_characteristic->getValueHandle();
                    return;
                } 
            }


            // if this is a new sensor type
            dev.chars.sensor_chars[dev.num_of_chars].type = sensor_char_uuid_list[i].type;   
            dev.chars.sensor_chars[dev.num_of_chars].data_handle =  discovered_characteristic->getValueHandle(); 
            
            print_device_info(dev);
            
            dev.num_of_chars++;
            return;

        }
    }
}



void GattClientProcess::when_char_data_read(const GattReadCallbackParams* params)
{
 
    // get attribute handle, data and data length 
    GattAttribute::Handle_t data_handle = params->handle;
    ble::connection_handle_t connHandle = params->connHandle;
    const uint16_t len = params->len;
    const uint8_t* p_data = params->data;

        // Check read status
    if(params->status != BLE_ERROR_NONE){
        printf("Status %x during read data from data handle %d. Error code %x.\n",
                params->status,
                connHandle,
                data_handle,
                params->error_code);
        _event_queue->call(this,&Self::stop_service_discovery);        
        return;
    }
    Device_t &dev = devices[connHandle];

    printf("read %d bytes from [connection handle] %d [data handle] %d\n",len,connHandle,data_handle);

    // read data from callback parameter
    for(uint16_t i = 0; i < len;i++){
        printf("0x%02x ",p_data[i]);
    }
    printf("\n");

        // if device has CTS
    if(dev.is_CTC){
        //setRTC(p_data,len);
        _event_queue->call<GattClientProcess,void,const uint8_t*,uint16_t>(this,&Self::setRTC,p_data,len);   
        //_event_queue->call<GattClientProcess,void,Device_t&>(this,&Self::disconnect_peer,dev);
        return;
    }

    if(dev.is_beacon){
        // check what sensor type does this handle related to
        for(size_t i = 0; i < dev.num_of_chars;i++){
                Sensor_char_t* sensor_char = &dev.chars.sensor_chars[i];
                if(data_handle == sensor_char->data_handle){
                    switch (sensor_char->type)
                    {
                    case Sensor_type::light:
                        process_light_sensor_data(params->data,sensor_char);
                        break;
                    case Sensor_type::magnetometer:
                        process_magnetometer_sensor_data(params->data,sensor_char);
                        break;
                    default:
                        break;
                    }

                    if(i < dev.num_of_chars - 1 ){
                        // if it is not the last characteristic, read next characteristic.
                       _event_queue->call<Self,void,ble::connection_handle_t,GattAttribute::Handle_t>(
                                this,&Self::read_data_from_handle,dev.connection_handle,dev.chars.sensor_chars[i+1].data_handle);
                    }else{
                        
                        // if it is the last characteristic, write data to sd card, then wait a period and read next characteristic.
                        printf("send data to SD card: ");
                        for(int i = 0; i < sensor_char->len;i++){
                            printf("%d ",sensor_char->data[i]);
                        }
                        printf("\n");
                        for(int i = 0; i < _count_cb;i++){
                            _post_read_sensor_value_complete_cb[i](dev);
                        }
                        printf("Finish read all characters.\n");
                        
                        _event_queue->call_in<Self,void,ble::connection_handle_t,GattAttribute::Handle_t>(
                            PERIOD_READ_BEACON_DATA,this,&Self::read_data_from_handle,dev.connection_handle,dev.chars.sensor_chars[0].data_handle);  

                    }
            }
        }
    }

}

void GattClientProcess::process_magnetometer_sensor_data(const uint8_t *p_data,Sensor_char_t* p_char)
{
    p_char->data[0] = (uint16_t)p_data[13] << 8 | (uint16_t)p_data[12];
    p_char->data[1] = (uint16_t)p_data[15] << 8 | (uint16_t)p_data[14];
    p_char->data[2] = (uint16_t)p_data[17] << 8 | (uint16_t)p_data[16];
    p_char->len = 3;
}

void GattClientProcess::process_light_sensor_data(const uint8_t *p_data,Sensor_char_t* p_char)
{
        // if get a 2-bytes light value, convert it to lux
        int16_t rawData, e,m;

        // byte 1 is MSB, byte 0 is LSB
        rawData = (int16_t)p_data[1] << 8 | (int16_t)p_data[0];
        m = rawData & 0x0FFF;
        e = (rawData & 0xF000) >> 12;

        /** e on 4 bits stored in a 16 bit unsigned => it can store 2 << (e - 1) with e < 16 */
        // e = (e == 0) ? 1 : 2 << (e - 1);
        p_char->data[0] = m * (0.01 * exp2(e));
        p_char->len = 1;
}


// Stop the discovery process and clean the instance.
void GattClientProcess::stop_service_discovery()
{  
    if(_ble_interface == NULL){
        printf("NO ble interface.\n");
        return;
    }

    if (!_gattClient) {
        printf("no gatt instance\n");
         return;
    }

    // unregister event handlers
    _gattClient->onServiceDiscoveryTermination(NULL);

    // Clear up instance
    _gattClient = NULL;

    printf("Service discovery stopped.\n");
}

void GattClientProcess::disconnect_peer(Device_t &dev){
    ble_error_t error;
    
    error = _ble_interface->gap().disconnect(
        dev.connection_handle,
        ble::local_disconnection_reason_t::USER_TERMINATION
    );

    if(error){
        printf("Error %s during disconnect device. info:\n",BLE::errorToString(error));
        print_device_info(dev);
        return;
    }
    printf("OK\n");
}

void GattClientProcess::setRTC(const uint8_t *p_data,uint16_t len){
    
        uint8_t data[len];
        memcpy(data,p_data,len);

        DateTime_t time;
        time.year           =  *((uint16_t*)data);
        time.month          = data[2];
        time.day            = data[3];
        time.hour           = data[4];
        time.minute         = data[5];
        time.second         = data[6];
        time.day_of_week    = data[7];

        printf("%d-%d-%d %d:%d:%d %d\n",
                time.year,
                time.month,
                time.day,
                time.hour,
                time.minute,
                time.second,
                time.day_of_week);

        
        struct tm when = {0};

        when.tm_hour = time.hour;
        when.tm_min = time.minute;
        when.tm_sec = time.second;

        when.tm_year = time.year - 1900;
        when.tm_mon = time.month - 1; // month since janury
        when.tm_mday = time.day;
        when.tm_isdst = 1; //Daylight Saving Time flag effect

        char buffer[100];

        time_t convert = mktime(&when);  // t is now your desired time_t
        set_time(convert);

        // When finish setting time, disconnect
        printf("Finish reading target characteristic.\n");

        // empty buffer
        memset(buffer,0,sizeof(buffer));

        // get current time from system
        time_t _current_time = std::time(NULL);
        
        //printf("Time as a basic string = %s", ctime(&_current_time));
        // "H"hour(24h)|"M"minute(00-59)|"S"second(00-60)|"d"day(01-31)| "m"month |"Y"years| "u"day of week(1-7) | 
        strftime(buffer,sizeof(buffer),"%Y-%m-%d %H:%M:%S  day of week %u",localtime(&_current_time));
        printf("local time %s\r\n",buffer);

}

void GattClientProcess::print_uuid(const UUID &uuid)
{
    const uint8_t* p_uuid = uuid.getBaseUUID();
    uint8_t len = uuid.getLen();
    // uuid is LSB
    for(int i = len - 1; i >-1; i--){
            printf("%.2x",p_uuid[i]);
    }
    printf("\n");
}


template<typename Arg>
FunctionPointerWithContext<Arg> GattClientProcess::as_cb(void (Self::*member)(Arg))
{
    return makeFunctionPointer(this, member);
}


void GattClientProcess::print_device_info(Device_t &dev){
    printf("\ndevice info: \ndev.connection_handle = %d\n",dev.connection_handle);
    printf("dev.is_CTC = %d\n",dev.is_CTC);
    printf("dev.is_beacon = %d\n",dev.is_beacon);
    printf("dev.num_of_chars = %d\n",dev.num_of_chars);
    printf("dev.address = ");
    print_addr(dev.address);
    printf("\n");

    if(dev.is_CTC){
        printf("time value handle = 0x%x\n",dev.chars.time_value_handle);
    }

    if(dev.is_beacon){
        printf("size = %d\n",dev.num_of_chars);
        for(size_t i = 0; i < dev.num_of_chars ; i++){
            printf("[sensor type] %d [data handle] %d [config handle] %d\n",
                                    dev.chars.sensor_chars[i].type, 
                                    dev.chars.sensor_chars[i].data_handle,
                                    dev.chars.sensor_chars[i].config_handle);
        }
    }
    printf("\n");
}


void GattClientProcess::print_addr(const uint8_t *addr)
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}
