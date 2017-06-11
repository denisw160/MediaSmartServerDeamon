/////////////////////////////////////////////////////////////////////////////
/// @file device_monitor.cpp
///
/// Device monitoring (disk add/removal etc)
///
/// -------------------------------------------------------------------------
///
/// Copyright (c) 2009-2010 Chris Byrne
/// 
/// This software is provided 'as-is', without any express or implied
/// warranty. In no event will the authors be held liable for any damages
/// arising from the use of this software.
/// 
/// Permission is granted to anyone to use this software for any purpose,
/// including commercial applications, and to alter it and redistribute it
/// freely, subject to the following restrictions:
/// 
/// 1. The origin of this software must not be misrepresented; you must not
/// claim that you wrote the original software. If you use this software
/// in a product, an acknowledgment in the product documentation would be
/// appreciated but is not required.
/// 
/// 2. Altered source versions must be plainly marked as such, and must not
/// be misrepresented as being the original software.
/// 
/// 3. This notice may not be removed or altered from any source
/// distribution.
///
/////////////////////////////////////////////////////////////////////////////

//- includes
#include "device_monitor.h"
#include "errno_exception.h"
#include "mediasmartserverd.h"
#include <iostream>
#include <map>
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
extern "C" {
#include <libudev.h>
}

//- types
typedef std::tr1::shared_ptr< udev_device > UdevDevicePtr;

/////////////////////////////////////////////////////////////////////////////
/// constructor
DeviceMonitor::DeviceMonitor( )
	:	dev_context_( 0 )
	,	dev_monitor_( 0 )
	,	led_index_ofs_( 0 )
{ }
	
/////////////////////////////////////////////////////////////////////////////
/// destructor
DeviceMonitor::~DeviceMonitor( ) {
	if ( dev_context_ ) udev_unref( dev_context_ );
	if ( dev_monitor_ ) udev_monitor_unref( dev_monitor_ );
}

/////////////////////////////////////////////////////////////////////////////
/// intialise
void DeviceMonitor::Init( const LedControlPtr& leds ) {
	leds_ = leds;
	
	// get udev library context
	dev_context_ = udev_new();
	if ( !dev_context_ ) throw ErrnoException( "udev_new" );
	
	// set up udev monitor
	dev_monitor_ = udev_monitor_new_from_netlink( dev_context_, "udev" );
	if ( !dev_monitor_ ) throw ErrnoException( "udev_monitor_new_from_netlink" );
	
	// only interested in scsi devices
	if ( udev_monitor_filter_add_match_subsystem_devtype( dev_monitor_, "scsi", "scsi_device" ) ) {
		throw ErrnoException( "udev_monitor_filter_add_match_subsystem_devtype" );
	}
	
	// enumerate existing devices
	if ( verbose ) std::cout << "Enumerating attached devices...\n";
	enumDevices_( );
	
	// then start monitoring
	if ( verbose ) std::cout << "Monitoring devices...\n";
	if ( udev_monitor_enable_receiving( dev_monitor_ ) ) {
		throw ErrnoException( "udev_monitor_enable_receiving" );
	}
}

/////////////////////////////////////////////////////////////////////////////
/// main looop
void DeviceMonitor::Main( ) {
	assert( dev_monitor_ );
	
	const int fd_mon = udev_monitor_get_fd( dev_monitor_ );
	const int nfds = fd_mon + 1;
	
	sigset_t sigempty;
	sigemptyset( &sigempty );
	while ( true ) {
		fd_set fds_read;
		FD_ZERO( &fds_read );
		FD_SET( fd_mon, &fds_read );
		
		// block for something interesting to happen
		int res = pselect( nfds, &fds_read, 0, 0, 0, &sigempty );
		if ( res < 0 ) {
			if ( EINTR != errno ) throw ErrnoException( "select" );
			std::cout << "Exiting on signal\n";
			return; // signalled
		}
		
		// udev monitor notification?
		if ( FD_ISSET( fd_mon, &fds_read ) ) {
			UdevDevicePtr device( udev_monitor_receive_device( dev_monitor_ ), &udev_device_unref );
			
			const char* str = udev_device_get_action( device.get() );
			if ( !str ) {
			} else if ( 0 == strcasecmp( str, "add" ) ) {
				deviceAdded_( device.get() );
			} else if ( 0 == strcasecmp( str, "remove" ) ) {
				deviceRemove_( device.get() );
			} else {
				if ( debug ) {
					std::cout << "action: " << str << '\n';
					std::cout << ' ' << udev_device_get_syspath(device.get()) << "' (" << udev_device_get_subsystem(device.get()) << ")\n";
				}
			}
		}
	}
}

/////////////////////////////////////////////////////////////////////////////
/// device added
void DeviceMonitor::deviceAdded_( udev_device* device ) {
	deviceChanged_( device, true );
}

/////////////////////////////////////////////////////////////////////////////
/// device removed
void DeviceMonitor::deviceRemove_( udev_device* device ) {
	deviceChanged_( device, false );
}

/////////////////////////////////////////////////////////////////////////////
/// device has changed
void DeviceMonitor::deviceChanged_( udev_device* device, bool state, int led_idx ) {
	if ( debug || verbose > 1 ) std::cout << "Device " << (state ? "added" : "removed") << " '" << udev_device_get_syspath(device) << "'\n";
	
	// retrieve LED index if needed
	if ( led_idx <= 0 ) led_idx = getLedIndexForDevice_( device );
	if ( led_idx <= 0 ) return;
	
	const char* model = udev_device_get_sysattr_value( device, "model" );
	std::cout << (state ? "ADDED" : "REMOVED") << " [" << led_idx << "] '" << ((model) ? udev_device_get_sysattr_value(device, "model") : "") << "'\n";
	
	// set the appopriate LED
	if ( leds_ ) leds_->Set( LED_BLUE, led_idx - 1, state );
}

/////////////////////////////////////////////////////////////////////////////
/// retrieve LED index for device
/// @returns led index or <= 0 if device is not the drive we are looking for
int DeviceMonitor::getLedIndexForDevice_( udev_device* device ) {
	// find the scsi_host that device is on
	udev_device* scsi_host = udev_device_get_parent_with_subsystem_devtype( device, "scsi", "scsi_host" );
	if ( !scsi_host ) return 0;
	
	if ( debug ) std::cout << " scsi_host: '" << udev_device_get_syspath(scsi_host) << "' (" << udev_device_get_subsystem(scsi_host) << ")\n";
	
	// system number indicates which bay
	const char* sysnum = udev_device_get_sysnum( scsi_host );
	if ( !sysnum ) return 0;
	
	if ( debug || verbose > 1 ) std::cout << " sysnum: " << sysnum << '\n';
	const int led_idx = atoi( sysnum ) - led_index_ofs_ + 1;
	
	// retrieve device parent
	udev_device* scsi_host_parent = udev_device_get_parent( scsi_host );
	if ( !scsi_host_parent ) return 0;
	
	if ( debug ) std::cout << " scsi_host_parent: '" << udev_device_get_syspath(scsi_host_parent) << '\n';
    
    // retrieve parent subsystem
    const char* scsi_host_parent_subsystem = udev_device_get_subsystem(scsi_host_parent);
    if ( !scsi_host_parent_subsystem ) return led_idx; // could be NULL - #2 Acer H340 segfaults with kernel 3.5.0
    
    if ( debug ) std::cout << " subsystem: " << scsi_host_parent_subsystem << '\n';
	
	// ensure that scsi_host is attached to PCI (and not say USB)
	return ( 0 == strcmp("pci", scsi_host_parent_subsystem) )
		?  led_idx
		: -led_idx
	;
}

/////////////////////////////////////////////////////////////////////////////
/// enumerate existing devices
void DeviceMonitor::enumDevices_( ) {
	assert( dev_context_ );
	
	// create udev enumeration interface
	std::tr1::shared_ptr< udev_enumerate > dev_enum( udev_enumerate_new( dev_context_ ), &udev_enumerate_unref );
	
	// only interested in scsi_device's
	udev_enumerate_add_match_property( dev_enum.get(), "DEVTYPE", "scsi_device" );
	udev_enumerate_scan_devices( dev_enum.get() ); // start
	
	// list of devices (ordered by their sequence number)
	typedef std::map< int, UdevDevicePtr > ListDevices;
	ListDevices scsi_devices;
	
	//- enumerate list (assumes that this is ordered sequentially for us already)
	udev_list_entry* list_entry = udev_enumerate_get_list_entry( dev_enum.get() );
	for ( ; list_entry; list_entry = udev_list_entry_get_next( list_entry ) ) {
		// retrieve device
		UdevDevicePtr device(
			udev_device_new_from_syspath(
				udev_enumerate_get_udev( dev_enum.get() ),
				udev_list_entry_get_name( list_entry )
			), &udev_device_unref
		);
		if ( !device ) continue;
		
		//	
		if ( debug || verbose > 1 ) std::cout << "Device '" << udev_device_get_syspath(device.get()) << "'\n";
		
		// retrieve led index
		const int led_idx = getLedIndexForDevice_( device.get() );
		if ( 0 == led_idx ) continue; // invalid device (missing information)
		if ( led_idx < 0 ) device.reset(); // invalid device (USB stick or something?)
		
		// add to our ordered map
		scsi_devices.insert( std::make_pair( abs(led_idx), device ) );
	}
	
	// iterate collected scsi devices
	bool found_valid = false;
	for ( ListDevices::const_iterator it = scsi_devices.begin(); it != scsi_devices.end(); ++it ) {
		if ( !it->second ) {
			// update led base offset to account for invalid devices
			if ( !found_valid ) led_index_ofs_ = it->first;
			continue;
		}
		if ( !found_valid && debug ) std::cout << "led_index_ofs = " << led_index_ofs_ << '\n';
		found_valid = true;
		
		deviceChanged_( it->second.get(), true, it->first );
	}
}
