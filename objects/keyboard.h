/*! \file Keyboard.h
	Defines the class Keyboard
*/
/************************************************************************************************
Copyright 2008 Gregory W Heckler

This file is part of the GPS Software Defined Radio (GPS-SDR)

The GPS-SDR is free software; you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation; either version 2 of the
License, or (at your option) any later version.

The GPS-SDR is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License along with GPS-SDR; if not,
write to the: 

Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
************************************************************************************************/

#ifndef Keyboard_H
#define Keyboard_H

#include "includes.h"

/*! \ingroup CLASSES
 * 
 */
class Keyboard
{

	private:

		pthread_t thread;	//!< For the thread
		int32 temp;

	public:

		Keyboard();
		~Keyboard();
		void Inport();
		void Parse();
		void Export();
		void Start();
		void Stop();

};

#endif /* Keyboard_H */
