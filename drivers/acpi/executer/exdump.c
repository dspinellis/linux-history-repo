/******************************************************************************
 *
 * Module Name: exdump - Interpreter debug output routines
 *
 *****************************************************************************/

/*
 * Copyright (C) 2000 - 2006, R. Byron Moore
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 */

#include <acpi/acpi.h>
#include <acpi/acinterp.h>
#include <acpi/amlcode.h>
#include <acpi/acnamesp.h>
#include <acpi/acparser.h>

#define _COMPONENT          ACPI_EXECUTER
ACPI_MODULE_NAME("exdump")

/*
 * The following routines are used for debug output only
 */
#if defined(ACPI_DEBUG_OUTPUT) || defined(ACPI_DEBUGGER)
/* Local prototypes */
static void acpi_ex_out_string(char *title, char *value);

static void acpi_ex_out_pointer(char *title, void *value);

static void acpi_ex_out_address(char *title, acpi_physical_address value);

static void
acpi_ex_dump_object(union acpi_operand_object *obj_desc,
		    struct acpi_exdump_info *info);

static void acpi_ex_dump_reference_obj(union acpi_operand_object *obj_desc);

static void
acpi_ex_dump_package_obj(union acpi_operand_object *obj_desc,
			 u32 level, u32 index);

/*******************************************************************************
 *
 * Object Descriptor info tables
 *
 * Note: The first table entry must be an INIT opcode and must contain
 * the table length (number of table entries)
 *
 ******************************************************************************/

static struct acpi_exdump_info acpi_ex_dump_integer[2] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_integer), NULL},
	{ACPI_EXD_UINT64, ACPI_EXD_OFFSET(integer.value), "Value"}
};

static struct acpi_exdump_info acpi_ex_dump_string[4] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_string), NULL},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(string.length), "Length"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(string.pointer), "Pointer"},
	{ACPI_EXD_STRING, 0, NULL}
};

static struct acpi_exdump_info acpi_ex_dump_buffer[4] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_buffer), NULL},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(buffer.length), "Length"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(buffer.pointer), "Pointer"},
	{ACPI_EXD_BUFFER, 0, NULL}
};

static struct acpi_exdump_info acpi_ex_dump_package[5] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_package), NULL},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(package.flags), "Flags"},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(package.count), "Elements"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(package.elements), "Element List"},
	{ACPI_EXD_PACKAGE, 0, NULL}
};

static struct acpi_exdump_info acpi_ex_dump_device[4] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_device), NULL},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(device.handler), "Handler"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(device.system_notify),
	 "System Notify"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(device.device_notify),
	 "Device Notify"}
};

static struct acpi_exdump_info acpi_ex_dump_event[2] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_event), NULL},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(event.semaphore), "Semaphore"}
};

static struct acpi_exdump_info acpi_ex_dump_method[8] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_method), NULL},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(method.param_count), "param_count"},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(method.concurrency), "Concurrency"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(method.semaphore), "Semaphore"},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(method.owner_id), "Owner Id"},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(method.thread_count), "Thread Count"},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(method.aml_length), "Aml Length"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(method.aml_start), "Aml Start"}
};

static struct acpi_exdump_info acpi_ex_dump_mutex[5] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_mutex), NULL},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(mutex.sync_level), "Sync Level"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(mutex.owner_thread), "Owner Thread"},
	{ACPI_EXD_UINT16, ACPI_EXD_OFFSET(mutex.acquisition_depth),
	 "Acquire Depth"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(mutex.semaphore), "Semaphore"}
};

static struct acpi_exdump_info acpi_ex_dump_region[7] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_region), NULL},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(region.space_id), "Space Id"},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(region.flags), "Flags"},
	{ACPI_EXD_ADDRESS, ACPI_EXD_OFFSET(region.address), "Address"},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(region.length), "Length"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(region.handler), "Handler"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(region.next), "Next"}
};

static struct acpi_exdump_info acpi_ex_dump_power[5] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_power), NULL},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(power_resource.system_level),
	 "System Level"},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(power_resource.resource_order),
	 "Resource Order"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(power_resource.system_notify),
	 "System Notify"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(power_resource.device_notify),
	 "Device Notify"}
};

static struct acpi_exdump_info acpi_ex_dump_processor[7] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_processor), NULL},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(processor.proc_id), "Processor ID"},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(processor.length), "Length"},
	{ACPI_EXD_ADDRESS, ACPI_EXD_OFFSET(processor.address), "Address"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(processor.system_notify),
	 "System Notify"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(processor.device_notify),
	 "Device Notify"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(processor.handler), "Handler"}
};

static struct acpi_exdump_info acpi_ex_dump_thermal[4] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_thermal), NULL},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(thermal_zone.system_notify),
	 "System Notify"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(thermal_zone.device_notify),
	 "Device Notify"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(thermal_zone.handler), "Handler"}
};

static struct acpi_exdump_info acpi_ex_dump_buffer_field[3] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_buffer_field), NULL},
	{ACPI_EXD_FIELD, 0, NULL},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(buffer_field.buffer_obj),
	 "Buffer Object"}
};

static struct acpi_exdump_info acpi_ex_dump_region_field[3] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_region_field), NULL},
	{ACPI_EXD_FIELD, 0, NULL},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(field.region_obj), "Region Object"}
};

static struct acpi_exdump_info acpi_ex_dump_bank_field[5] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_bank_field), NULL},
	{ACPI_EXD_FIELD, 0, NULL},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(bank_field.value), "Value"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(bank_field.region_obj),
	 "Region Object"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(bank_field.bank_obj), "Bank Object"}
};

static struct acpi_exdump_info acpi_ex_dump_index_field[5] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_bank_field), NULL},
	{ACPI_EXD_FIELD, 0, NULL},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(index_field.value), "Value"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(index_field.index_obj),
	 "Index Object"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(index_field.data_obj), "Data Object"}
};

static struct acpi_exdump_info acpi_ex_dump_reference[7] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_reference), NULL},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(reference.target_type), "Target Type"},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(reference.offset), "Offset"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(reference.object), "Object Desc"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(reference.node), "Node"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(reference.where), "Where"},
	{ACPI_EXD_REFERENCE, 0, NULL}
};

static struct acpi_exdump_info acpi_ex_dump_address_handler[6] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_address_handler),
	 NULL},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(address_space.space_id), "Space Id"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(address_space.next), "Next"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(address_space.region_list),
	 "Region List"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(address_space.node), "Node"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(address_space.context), "Context"}
};

static struct acpi_exdump_info acpi_ex_dump_notify[3] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_notify), NULL},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(notify.node), "Node"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(notify.context), "Context"}
};

/* Miscellaneous tables */

static struct acpi_exdump_info acpi_ex_dump_common[4] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_common), NULL},
	{ACPI_EXD_TYPE, 0, NULL},
	{ACPI_EXD_UINT16, ACPI_EXD_OFFSET(common.reference_count),
	 "Reference Count"},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(common.flags), "Flags"}
};

static struct acpi_exdump_info acpi_ex_dump_field_common[7] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_field_common), NULL},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(common_field.field_flags),
	 "Field Flags"},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(common_field.access_byte_width),
	 "Access Byte Width"},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(common_field.bit_length),
	 "Bit Length"},
	{ACPI_EXD_UINT8, ACPI_EXD_OFFSET(common_field.start_field_bit_offset),
	 "Field Bit Offset"},
	{ACPI_EXD_UINT32, ACPI_EXD_OFFSET(common_field.base_byte_offset),
	 "Base Byte Offset"},
	{ACPI_EXD_POINTER, ACPI_EXD_OFFSET(common_field.node), "Parent Node"}
};

static struct acpi_exdump_info acpi_ex_dump_node[5] = {
	{ACPI_EXD_INIT, ACPI_EXD_TABLE_SIZE(acpi_ex_dump_node), NULL},
	{ACPI_EXD_UINT8, ACPI_EXD_NSOFFSET(flags), "Flags"},
	{ACPI_EXD_UINT8, ACPI_EXD_NSOFFSET(owner_id), "Owner Id"},
	{ACPI_EXD_POINTER, ACPI_EXD_NSOFFSET(child), "Child List"},
	{ACPI_EXD_POINTER, ACPI_EXD_NSOFFSET(peer), "Next Peer"}
};

/* Dispatch table, indexed by object type */

static struct acpi_exdump_info *acpi_ex_dump_info[] = {
	NULL,
	acpi_ex_dump_integer,
	acpi_ex_dump_string,
	acpi_ex_dump_buffer,
	acpi_ex_dump_package,
	NULL,
	acpi_ex_dump_device,
	acpi_ex_dump_event,
	acpi_ex_dump_method,
	acpi_ex_dump_mutex,
	acpi_ex_dump_region,
	acpi_ex_dump_power,
	acpi_ex_dump_processor,
	acpi_ex_dump_thermal,
	acpi_ex_dump_buffer_field,
	NULL,
	NULL,
	acpi_ex_dump_region_field,
	acpi_ex_dump_bank_field,
	acpi_ex_dump_index_field,
	acpi_ex_dump_reference,
	NULL,
	NULL,
	acpi_ex_dump_notify,
	acpi_ex_dump_address_handler,
	NULL,
	NULL,
	NULL
};

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_dump_object
 *
 * PARAMETERS:  obj_desc            - Descriptor to dump
 *              Info                - Info table corresponding to this object
 *                                    type
 *
 * RETURN:      None
 *
 * DESCRIPTION: Walk the info table for this object
 *
 ******************************************************************************/

static void
acpi_ex_dump_object(union acpi_operand_object *obj_desc,
		    struct acpi_exdump_info *info)
{
	u8 *target;
	char *name;
	u8 count;

	if (!info) {
		acpi_os_printf
		    ("ex_dump_object: Display not implemented for object type %s\n",
		     acpi_ut_get_object_type_name(obj_desc));
		return;
	}

	/* First table entry must contain the table length (# of table entries) */

	count = info->offset;

	while (count) {
		target = ACPI_ADD_PTR(u8, obj_desc, info->offset);
		name = info->name;

		switch (info->opcode) {
		case ACPI_EXD_INIT:
			break;

		case ACPI_EXD_TYPE:
			acpi_ex_out_string("Type",
					   acpi_ut_get_object_type_name
					   (obj_desc));
			break;

		case ACPI_EXD_UINT8:

			acpi_os_printf("%20s : %2.2X\n", name, *target);
			break;

		case ACPI_EXD_UINT16:

			acpi_os_printf("%20s : %4.4X\n", name,
				       ACPI_GET16(target));
			break;

		case ACPI_EXD_UINT32:

			acpi_os_printf("%20s : %8.8X\n", name,
				       ACPI_GET32(target));
			break;

		case ACPI_EXD_UINT64:

			acpi_os_printf("%20s : %8.8X%8.8X\n", "Value",
				       ACPI_FORMAT_UINT64(ACPI_GET64(target)));
			break;

		case ACPI_EXD_POINTER:

			acpi_ex_out_pointer(name,
					    *ACPI_CAST_PTR(void *, target));
			break;

		case ACPI_EXD_ADDRESS:

			acpi_ex_out_address(name,
					    *ACPI_CAST_PTR
					    (acpi_physical_address, target));
			break;

		case ACPI_EXD_STRING:

			acpi_ut_print_string(obj_desc->string.pointer,
					     ACPI_UINT8_MAX);
			acpi_os_printf("\n");
			break;

		case ACPI_EXD_BUFFER:

			ACPI_DUMP_BUFFER(obj_desc->buffer.pointer,
					 obj_desc->buffer.length);
			break;

		case ACPI_EXD_PACKAGE:

			/* Dump the package contents */

			acpi_os_printf("\nPackage Contents:\n");
			acpi_ex_dump_package_obj(obj_desc, 0, 0);
			break;

		case ACPI_EXD_FIELD:

			acpi_ex_dump_object(obj_desc,
					    acpi_ex_dump_field_common);
			break;

		case ACPI_EXD_REFERENCE:

			acpi_ex_out_string("Opcode",
					   (acpi_ps_get_opcode_info
					    (obj_desc->reference.opcode))->
					   name);
			acpi_ex_dump_reference_obj(obj_desc);
			break;

		default:
			acpi_os_printf("**** Invalid table opcode [%X] ****\n",
				       info->opcode);
			return;
		}

		info++;
		count--;
	}
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_dump_operand
 *
 * PARAMETERS:  *obj_desc       - Pointer to entry to be dumped
 *              Depth           - Current nesting depth
 *
 * RETURN:      None
 *
 * DESCRIPTION: Dump an operand object
 *
 ******************************************************************************/

void acpi_ex_dump_operand(union acpi_operand_object *obj_desc, u32 depth)
{
	u32 length;
	u32 index;

	ACPI_FUNCTION_NAME("ex_dump_operand")

	    if (!
		((ACPI_LV_EXEC & acpi_dbg_level)
		 && (_COMPONENT & acpi_dbg_layer))) {
		return;
	}

	if (!obj_desc) {

		/* This could be a null element of a package */

		ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "Null Object Descriptor\n"));
		return;
	}

	if (ACPI_GET_DESCRIPTOR_TYPE(obj_desc) == ACPI_DESC_TYPE_NAMED) {
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "%p Namespace Node: ",
				  obj_desc));
		ACPI_DUMP_ENTRY(obj_desc, ACPI_LV_EXEC);
		return;
	}

	if (ACPI_GET_DESCRIPTOR_TYPE(obj_desc) != ACPI_DESC_TYPE_OPERAND) {
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "%p is not a node or operand object: [%s]\n",
				  obj_desc,
				  acpi_ut_get_descriptor_name(obj_desc)));
		ACPI_DUMP_BUFFER(obj_desc, sizeof(union acpi_operand_object));
		return;
	}

	/* obj_desc is a valid object */

	if (depth > 0) {
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "%*s[%u] %p ",
				  depth, " ", depth, obj_desc));
	} else {
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "%p ", obj_desc));
	}

	/* Decode object type */

	switch (ACPI_GET_OBJECT_TYPE(obj_desc)) {
	case ACPI_TYPE_LOCAL_REFERENCE:

		switch (obj_desc->reference.opcode) {
		case AML_DEBUG_OP:

			acpi_os_printf("Reference: Debug\n");
			break;

		case AML_NAME_OP:

			ACPI_DUMP_PATHNAME(obj_desc->reference.object,
					   "Reference: Name: ", ACPI_LV_INFO,
					   _COMPONENT);
			ACPI_DUMP_ENTRY(obj_desc->reference.object,
					ACPI_LV_INFO);
			break;

		case AML_INDEX_OP:

			acpi_os_printf("Reference: Index %p\n",
				       obj_desc->reference.object);
			break;

		case AML_REF_OF_OP:

			acpi_os_printf("Reference: (ref_of) %p\n",
				       obj_desc->reference.object);
			break;

		case AML_ARG_OP:

			acpi_os_printf("Reference: Arg%d",
				       obj_desc->reference.offset);

			if (ACPI_GET_OBJECT_TYPE(obj_desc) == ACPI_TYPE_INTEGER) {

				/* Value is an Integer */

				acpi_os_printf(" value is [%8.8X%8.8x]",
					       ACPI_FORMAT_UINT64(obj_desc->
								  integer.
								  value));
			}

			acpi_os_printf("\n");
			break;

		case AML_LOCAL_OP:

			acpi_os_printf("Reference: Local%d",
				       obj_desc->reference.offset);

			if (ACPI_GET_OBJECT_TYPE(obj_desc) == ACPI_TYPE_INTEGER) {

				/* Value is an Integer */

				acpi_os_printf(" value is [%8.8X%8.8x]",
					       ACPI_FORMAT_UINT64(obj_desc->
								  integer.
								  value));
			}

			acpi_os_printf("\n");
			break;

		case AML_INT_NAMEPATH_OP:

			acpi_os_printf("Reference.Node->Name %X\n",
				       obj_desc->reference.node->name.integer);
			break;

		default:

			/* Unknown opcode */

			acpi_os_printf("Unknown Reference opcode=%X\n",
				       obj_desc->reference.opcode);
			break;

		}
		break;

	case ACPI_TYPE_BUFFER:

		acpi_os_printf("Buffer len %X @ %p\n",
			       obj_desc->buffer.length,
			       obj_desc->buffer.pointer);

		length = obj_desc->buffer.length;
		if (length > 64) {
			length = 64;
		}

		/* Debug only -- dump the buffer contents */

		if (obj_desc->buffer.pointer) {
			acpi_os_printf("Buffer Contents: ");

			for (index = 0; index < length; index++) {
				acpi_os_printf(" %02x",
					       obj_desc->buffer.pointer[index]);
			}
			acpi_os_printf("\n");
		}
		break;

	case ACPI_TYPE_INTEGER:

		acpi_os_printf("Integer %8.8X%8.8X\n",
			       ACPI_FORMAT_UINT64(obj_desc->integer.value));
		break;

	case ACPI_TYPE_PACKAGE:

		acpi_os_printf("Package [Len %X] element_array %p\n",
			       obj_desc->package.count,
			       obj_desc->package.elements);

		/*
		 * If elements exist, package element pointer is valid,
		 * and debug_level exceeds 1, dump package's elements.
		 */
		if (obj_desc->package.count &&
		    obj_desc->package.elements && acpi_dbg_level > 1) {
			for (index = 0; index < obj_desc->package.count;
			     index++) {
				acpi_ex_dump_operand(obj_desc->package.
						     elements[index],
						     depth + 1);
			}
		}
		break;

	case ACPI_TYPE_REGION:

		acpi_os_printf("Region %s (%X)",
			       acpi_ut_get_region_name(obj_desc->region.
						       space_id),
			       obj_desc->region.space_id);

		/*
		 * If the address and length have not been evaluated,
		 * don't print them.
		 */
		if (!(obj_desc->region.flags & AOPOBJ_DATA_VALID)) {
			acpi_os_printf("\n");
		} else {
			acpi_os_printf(" base %8.8X%8.8X Length %X\n",
				       ACPI_FORMAT_UINT64(obj_desc->region.
							  address),
				       obj_desc->region.length);
		}
		break;

	case ACPI_TYPE_STRING:

		acpi_os_printf("String length %X @ %p ",
			       obj_desc->string.length,
			       obj_desc->string.pointer);

		acpi_ut_print_string(obj_desc->string.pointer, ACPI_UINT8_MAX);
		acpi_os_printf("\n");
		break;

	case ACPI_TYPE_LOCAL_BANK_FIELD:

		acpi_os_printf("bank_field\n");
		break;

	case ACPI_TYPE_LOCAL_REGION_FIELD:

		acpi_os_printf
		    ("region_field: Bits=%X acc_width=%X Lock=%X Update=%X at byte=%X bit=%X of below:\n",
		     obj_desc->field.bit_length,
		     obj_desc->field.access_byte_width,
		     obj_desc->field.field_flags & AML_FIELD_LOCK_RULE_MASK,
		     obj_desc->field.field_flags & AML_FIELD_UPDATE_RULE_MASK,
		     obj_desc->field.base_byte_offset,
		     obj_desc->field.start_field_bit_offset);

		acpi_ex_dump_operand(obj_desc->field.region_obj, depth + 1);
		break;

	case ACPI_TYPE_LOCAL_INDEX_FIELD:

		acpi_os_printf("index_field\n");
		break;

	case ACPI_TYPE_BUFFER_FIELD:

		acpi_os_printf("buffer_field: %X bits at byte %X bit %X of\n",
			       obj_desc->buffer_field.bit_length,
			       obj_desc->buffer_field.base_byte_offset,
			       obj_desc->buffer_field.start_field_bit_offset);

		if (!obj_desc->buffer_field.buffer_obj) {
			ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "*NULL*\n"));
		} else
		    if (ACPI_GET_OBJECT_TYPE(obj_desc->buffer_field.buffer_obj)
			!= ACPI_TYPE_BUFFER) {
			acpi_os_printf("*not a Buffer*\n");
		} else {
			acpi_ex_dump_operand(obj_desc->buffer_field.buffer_obj,
					     depth + 1);
		}
		break;

	case ACPI_TYPE_EVENT:

		acpi_os_printf("Event\n");
		break;

	case ACPI_TYPE_METHOD:

		acpi_os_printf("Method(%X) @ %p:%X\n",
			       obj_desc->method.param_count,
			       obj_desc->method.aml_start,
			       obj_desc->method.aml_length);
		break;

	case ACPI_TYPE_MUTEX:

		acpi_os_printf("Mutex\n");
		break;

	case ACPI_TYPE_DEVICE:

		acpi_os_printf("Device\n");
		break;

	case ACPI_TYPE_POWER:

		acpi_os_printf("Power\n");
		break;

	case ACPI_TYPE_PROCESSOR:

		acpi_os_printf("Processor\n");
		break;

	case ACPI_TYPE_THERMAL:

		acpi_os_printf("Thermal\n");
		break;

	default:
		/* Unknown Type */

		acpi_os_printf("Unknown Type %X\n",
			       ACPI_GET_OBJECT_TYPE(obj_desc));
		break;
	}

	return;
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_dump_operands
 *
 * PARAMETERS:  Operands            - Operand list
 *              interpreter_mode    - Load or Exec
 *              Ident               - Identification
 *              num_levels          - # of stack entries to dump above line
 *              Note                - Output notation
 *              module_name         - Caller's module name
 *              line_number         - Caller's invocation line number
 *
 * DESCRIPTION: Dump the object stack
 *
 ******************************************************************************/

void
acpi_ex_dump_operands(union acpi_operand_object **operands,
		      acpi_interpreter_mode interpreter_mode,
		      char *ident,
		      u32 num_levels,
		      char *note, char *module_name, u32 line_number)
{
	acpi_native_uint i;

	ACPI_FUNCTION_NAME("ex_dump_operands");

	if (!ident) {
		ident = "?";
	}

	if (!note) {
		note = "?";
	}

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
			  "************* Operand Stack Contents (Opcode [%s], %d Operands)\n",
			  ident, num_levels));

	if (num_levels == 0) {
		num_levels = 1;
	}

	/* Dump the operand stack starting at the top */

	for (i = 0; num_levels > 0; i--, num_levels--) {
		acpi_ex_dump_operand(operands[i], 0);
	}

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
			  "************* Operand Stack dump from %s(%d), %s\n",
			  module_name, line_number, note));
	return;
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_out* functions
 *
 * PARAMETERS:  Title               - Descriptive text
 *              Value               - Value to be displayed
 *
 * DESCRIPTION: Object dump output formatting functions.  These functions
 *              reduce the number of format strings required and keeps them
 *              all in one place for easy modification.
 *
 ******************************************************************************/

static void acpi_ex_out_string(char *title, char *value)
{
	acpi_os_printf("%20s : %s\n", title, value);
}

static void acpi_ex_out_pointer(char *title, void *value)
{
	acpi_os_printf("%20s : %p\n", title, value);
}

static void acpi_ex_out_address(char *title, acpi_physical_address value)
{

#if ACPI_MACHINE_WIDTH == 16
	acpi_os_printf("%20s : %p\n", title, value);
#else
	acpi_os_printf("%20s : %8.8X%8.8X\n", title, ACPI_FORMAT_UINT64(value));
#endif
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_dump_namespace_node
 *
 * PARAMETERS:  Node                - Descriptor to dump
 *              Flags               - Force display if TRUE
 *
 * DESCRIPTION: Dumps the members of the given.Node
 *
 ******************************************************************************/

void acpi_ex_dump_namespace_node(struct acpi_namespace_node *node, u32 flags)
{

	ACPI_FUNCTION_ENTRY();

	if (!flags) {
		if (!
		    ((ACPI_LV_OBJECTS & acpi_dbg_level)
		     && (_COMPONENT & acpi_dbg_layer))) {
			return;
		}
	}

	acpi_os_printf("%20s : %4.4s\n", "Name", acpi_ut_get_node_name(node));
	acpi_ex_out_string("Type", acpi_ut_get_type_name(node->type));
	acpi_ex_out_pointer("Attached Object",
			    acpi_ns_get_attached_object(node));
	acpi_ex_out_pointer("Parent", acpi_ns_get_parent_node(node));

	acpi_ex_dump_object(ACPI_CAST_PTR(union acpi_operand_object, node),
			    acpi_ex_dump_node);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_dump_reference_obj
 *
 * PARAMETERS:  Object              - Descriptor to dump
 *
 * DESCRIPTION: Dumps a reference object
 *
 ******************************************************************************/

static void acpi_ex_dump_reference_obj(union acpi_operand_object *obj_desc)
{
	struct acpi_buffer ret_buf;
	acpi_status status;

	ret_buf.length = ACPI_ALLOCATE_LOCAL_BUFFER;

	if (obj_desc->reference.opcode == AML_INT_NAMEPATH_OP) {
		acpi_os_printf("Named Object %p ", obj_desc->reference.node);

		status =
		    acpi_ns_handle_to_pathname(obj_desc->reference.node,
					       &ret_buf);
		if (ACPI_FAILURE(status)) {
			acpi_os_printf("Could not convert name to pathname\n");
		} else {
			acpi_os_printf("%s\n", (char *)ret_buf.pointer);
			ACPI_FREE(ret_buf.pointer);
		}
	} else if (obj_desc->reference.object) {
		acpi_os_printf("\nReferenced Object: %p\n",
			       obj_desc->reference.object);
	}
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_dump_package_obj
 *
 * PARAMETERS:  obj_desc            - Descriptor to dump
 *              Level               - Indentation Level
 *              Index               - Package index for this object
 *
 * DESCRIPTION: Dumps the elements of the package
 *
 ******************************************************************************/

static void
acpi_ex_dump_package_obj(union acpi_operand_object *obj_desc,
			 u32 level, u32 index)
{
	u32 i;

	/* Indentation and index output */

	if (level > 0) {
		for (i = 0; i < level; i++) {
			acpi_os_printf(" ");
		}

		acpi_os_printf("[%.2d] ", index);
	}

	acpi_os_printf("%p ", obj_desc);

	/* Null package elements are allowed */

	if (!obj_desc) {
		acpi_os_printf("[Null Object]\n");
		return;
	}

	/* Packages may only contain a few object types */

	switch (ACPI_GET_OBJECT_TYPE(obj_desc)) {
	case ACPI_TYPE_INTEGER:

		acpi_os_printf("[Integer] = %8.8X%8.8X\n",
			       ACPI_FORMAT_UINT64(obj_desc->integer.value));
		break;

	case ACPI_TYPE_STRING:

		acpi_os_printf("[String] Value: ");
		for (i = 0; i < obj_desc->string.length; i++) {
			acpi_os_printf("%c", obj_desc->string.pointer[i]);
		}
		acpi_os_printf("\n");
		break;

	case ACPI_TYPE_BUFFER:

		acpi_os_printf("[Buffer] Length %.2X = ",
			       obj_desc->buffer.length);
		if (obj_desc->buffer.length) {
			acpi_ut_dump_buffer(ACPI_CAST_PTR
					    (u8, obj_desc->buffer.pointer),
					    obj_desc->buffer.length,
					    DB_DWORD_DISPLAY, _COMPONENT);
		} else {
			acpi_os_printf("\n");
		}
		break;

	case ACPI_TYPE_PACKAGE:

		acpi_os_printf("[Package] Contains %d Elements:\n",
			       obj_desc->package.count);

		for (i = 0; i < obj_desc->package.count; i++) {
			acpi_ex_dump_package_obj(obj_desc->package.elements[i],
						 level + 1, i);
		}
		break;

	case ACPI_TYPE_LOCAL_REFERENCE:

		acpi_os_printf("[Object Reference] ");
		acpi_ex_dump_reference_obj(obj_desc);
		break;

	default:

		acpi_os_printf("[Unknown Type] %X\n",
			       ACPI_GET_OBJECT_TYPE(obj_desc));
		break;
	}
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_dump_object_descriptor
 *
 * PARAMETERS:  obj_desc            - Descriptor to dump
 *              Flags               - Force display if TRUE
 *
 * DESCRIPTION: Dumps the members of the object descriptor given.
 *
 ******************************************************************************/

void
acpi_ex_dump_object_descriptor(union acpi_operand_object *obj_desc, u32 flags)
{
	ACPI_FUNCTION_TRACE("ex_dump_object_descriptor");

	if (!obj_desc) {
		return_VOID;
	}

	if (!flags) {
		if (!
		    ((ACPI_LV_OBJECTS & acpi_dbg_level)
		     && (_COMPONENT & acpi_dbg_layer))) {
			return_VOID;
		}
	}

	if (ACPI_GET_DESCRIPTOR_TYPE(obj_desc) == ACPI_DESC_TYPE_NAMED) {
		acpi_ex_dump_namespace_node((struct acpi_namespace_node *)
					    obj_desc, flags);

		acpi_os_printf("\nAttached Object (%p):\n",
			       ((struct acpi_namespace_node *)obj_desc)->
			       object);

		acpi_ex_dump_object_descriptor(((struct acpi_namespace_node *)
						obj_desc)->object, flags);
		return_VOID;
	}

	if (ACPI_GET_DESCRIPTOR_TYPE(obj_desc) != ACPI_DESC_TYPE_OPERAND) {
		acpi_os_printf
		    ("ex_dump_object_descriptor: %p is not an ACPI operand object: [%s]\n",
		     obj_desc, acpi_ut_get_descriptor_name(obj_desc));
		return_VOID;
	}

	if (obj_desc->common.type > ACPI_TYPE_NS_NODE_MAX) {
		return_VOID;
	}

	/* Common Fields */

	acpi_ex_dump_object(obj_desc, acpi_ex_dump_common);

	/* Object-specific fields */

	acpi_ex_dump_object(obj_desc, acpi_ex_dump_info[obj_desc->common.type]);
	return_VOID;
}

#endif
