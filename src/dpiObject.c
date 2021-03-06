//-----------------------------------------------------------------------------
// Copyright (c) 2016, 2017 Oracle and/or its affiliates.  All rights reserved.
// This program is free software: you can modify it and/or redistribute it
// under the terms of:
//
// (i)  the Universal Permissive License v 1.0 or at your option, any
//      later version (http://oss.oracle.com/licenses/upl); and/or
//
// (ii) the Apache License v 2.0. (http://www.apache.org/licenses/LICENSE-2.0)
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// dpiObject.c
//   Implementation of objects.
//-----------------------------------------------------------------------------

#include "dpiImpl.h"

//-----------------------------------------------------------------------------
// dpiObject__allocate() [INTERNAL]
//   Allocate and initialize an object structure.
//-----------------------------------------------------------------------------
int dpiObject__allocate(dpiObjectType *objType, void *instance,
        void *indicator, int isIndependent, dpiObject **obj, dpiError *error)
{
    dpiObject *tempObj;

    if (dpiGen__allocate(DPI_HTYPE_OBJECT, objType->env, (void**) &tempObj,
            error) < 0)
        return DPI_FAILURE;
    if (dpiGen__setRefCount(objType, error, 1) < 0) {
        dpiObject__free(*obj, error);
        return DPI_FAILURE;
    }
    tempObj->type = objType;
    tempObj->instance = instance;
    tempObj->indicator = indicator;
    tempObj->isIndependent = isIndependent;
    *obj = tempObj;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObject__checkIsCollection() [INTERNAL]
//   Check if the object is a collection, and if not, raise an exception.
//-----------------------------------------------------------------------------
static int dpiObject__checkIsCollection(dpiObject *obj, const char *fnName,
        dpiError *error)
{
    if (dpiGen__startPublicFn(obj, DPI_HTYPE_OBJECT, fnName, error) < 0)
        return DPI_FAILURE;
    if (!obj->type->isCollection)
        return dpiError__set(error, "check collection", DPI_ERR_NOT_COLLECTION,
                obj->type->schemaLength, obj->type->schema,
                obj->type->nameLength, obj->type->name);
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObject__clearOracleValue() [INTERNAL]
//   Clear the Oracle value after use.
//-----------------------------------------------------------------------------
static void dpiObject__clearOracleValue(dpiEnv *env, dpiError *error,
        dpiOracleDataBuffer *buffer, dpiOracleTypeNum oracleTypeNum)
{
    switch (oracleTypeNum) {
        case DPI_ORACLE_TYPE_CHAR:
        case DPI_ORACLE_TYPE_VARCHAR:
            if (buffer->asString)
                dpiOci__stringResize(env, &buffer->asString, 0, error);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP:
            if (buffer->asTimestamp)
                dpiOci__descriptorFree(buffer->asTimestamp,
                        DPI_OCI_DTYPE_TIMESTAMP);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
            if (buffer->asTimestamp)
                dpiOci__descriptorFree(buffer->asTimestamp,
                        DPI_OCI_DTYPE_TIMESTAMP_TZ);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
            if (buffer->asTimestamp)
                dpiOci__descriptorFree(buffer->asTimestamp,
                        DPI_OCI_DTYPE_TIMESTAMP_LTZ);
            break;
        default:
            break;
    };
}


//-----------------------------------------------------------------------------
// dpiObject__free() [INTERNAL]
//   Free the memory for an object.
//-----------------------------------------------------------------------------
void dpiObject__free(dpiObject *obj, dpiError *error)
{
    if (obj->isIndependent) {
        dpiOci__objectFree(obj, error);
        obj->isIndependent = 0;
    }
    if (obj->type) {
        dpiGen__setRefCount(obj->type, error, -1);
        obj->type = NULL;
    }
    free(obj);
}


//-----------------------------------------------------------------------------
// dpiObject__fromOracleValue() [INTERNAL]
//   Populate data from the Oracle value or return an error if this is not
// possible.
//-----------------------------------------------------------------------------
static int dpiObject__fromOracleValue(dpiObject *obj, dpiError *error,
        const dpiDataTypeInfo *typeInfo, dpiOracleData *value,
        int16_t *indicator, dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    dpiOracleTypeNum valueOracleTypeNum;
    dpiBytes *asBytes;

    // null values are immediately returned (type is irrelevant)
    if (*indicator == DPI_OCI_IND_NULL) {
        data->isNull = 1;
        return DPI_SUCCESS;
    }

    // convert all other values
    data->isNull = 0;
    valueOracleTypeNum = typeInfo->oracleTypeNum;
    switch (valueOracleTypeNum) {
        case DPI_ORACLE_TYPE_CHAR:
        case DPI_ORACLE_TYPE_NCHAR:
        case DPI_ORACLE_TYPE_VARCHAR:
        case DPI_ORACLE_TYPE_NVARCHAR:
            if (nativeTypeNum == DPI_NATIVE_TYPE_BYTES) {
                asBytes = &data->value.asBytes;
                dpiOci__stringPtr(obj->env, *value->asString, &asBytes->ptr);
                dpiOci__stringSize(obj->env, *value->asString,
                        &asBytes->length);
                if (valueOracleTypeNum == DPI_ORACLE_TYPE_NCHAR ||
                        valueOracleTypeNum == DPI_ORACLE_TYPE_NVARCHAR)
                    asBytes->encoding = obj->env->nencoding;
                else asBytes->encoding = obj->env->encoding;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NATIVE_INT:
            if (nativeTypeNum == DPI_NATIVE_TYPE_INT64)
                return dpiDataBuffer__fromOracleNumberAsInteger(&data->value,
                        error, value->asNumber);
            break;
        case DPI_ORACLE_TYPE_NATIVE_FLOAT:
            if (nativeTypeNum == DPI_NATIVE_TYPE_FLOAT) {
                data->value.asFloat = *value->asFloat;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NATIVE_DOUBLE:
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE) {
                data->value.asDouble = *value->asDouble;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NUMBER:
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE)
                return dpiDataBuffer__fromOracleNumberAsDouble(&data->value,
                        error, value->asNumber);
            else if (nativeTypeNum == DPI_NATIVE_TYPE_INT64)
                return dpiDataBuffer__fromOracleNumberAsInteger(&data->value,
                        error, value->asNumber);
            else if (nativeTypeNum == DPI_NATIVE_TYPE_UINT64)
                return dpiDataBuffer__fromOracleNumberAsUnsignedInteger(
                        &data->value, error, value->asNumber);
            else if (nativeTypeNum == DPI_NATIVE_TYPE_BYTES)
                return dpiDataBuffer__fromOracleNumberAsText(&data->value,
                        obj->env, error, value->asNumber);
            break;
        case DPI_ORACLE_TYPE_DATE:
            if (nativeTypeNum == DPI_NATIVE_TYPE_TIMESTAMP)
                return dpiDataBuffer__fromOracleDate(&data->value,
                        value->asDate);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP:
            if (nativeTypeNum == DPI_NATIVE_TYPE_TIMESTAMP)
                return dpiDataBuffer__fromOracleTimestamp(&data->value,
                        obj->env, error, *value->asTimestamp, 0);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
        case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
            if (nativeTypeNum == DPI_NATIVE_TYPE_TIMESTAMP)
                return dpiDataBuffer__fromOracleTimestamp(&data->value,
                        obj->env, error, *value->asTimestamp, 1);
            break;
        case DPI_ORACLE_TYPE_OBJECT:
            if (typeInfo->objectType &&
                    nativeTypeNum == DPI_NATIVE_TYPE_OBJECT) {
                if (typeInfo->objectType->isCollection)
                    return dpiObject__allocate(typeInfo->objectType,
                            *value->asCollection, indicator, 0,
                            &data->value.asObject, error);
                return dpiObject__allocate(typeInfo->objectType, value->asRaw,
                        indicator, 0, &data->value.asObject, error);
            }
            break;
        case DPI_ORACLE_TYPE_BOOLEAN:
            if (nativeTypeNum == DPI_NATIVE_TYPE_BOOLEAN) {
                data->value.asBoolean = *(value->asBoolean);
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_CLOB:
        case DPI_ORACLE_TYPE_NCLOB:
        case DPI_ORACLE_TYPE_BLOB:
        case DPI_ORACLE_TYPE_BFILE:
            if (nativeTypeNum == DPI_NATIVE_TYPE_LOB) {
                dpiLob *tempLob;
                if (dpiGen__allocate(DPI_HTYPE_LOB, obj->env,
                        (void**) &tempLob, error) < 0)
                    return DPI_FAILURE;
                if (dpiGen__setRefCount(obj->type->conn, error, 1) < 0) {
                    dpiLob__free(tempLob, error);
                    return DPI_FAILURE;
                }
                tempLob->conn = obj->type->conn;
                tempLob->type = dpiOracleType__getFromNum(
                        typeInfo->oracleTypeNum, error);
                tempLob->locator = *(value->asLobLocator);
                data->value.asLOB = tempLob;
                return DPI_SUCCESS;
            }
            break;
        default:
            break;
    };

    return dpiError__set(error, "from Oracle value",
            DPI_ERR_UNHANDLED_CONVERSION, valueOracleTypeNum, nativeTypeNum);
}


//-----------------------------------------------------------------------------
// dpiObject__toOracleValue() [INTERNAL]
//   Convert value from external type to the OCI data type required.
//-----------------------------------------------------------------------------
static int dpiObject__toOracleValue(dpiObject *obj, dpiError *error,
        const dpiDataTypeInfo *dataTypeInfo, dpiOracleDataBuffer *buffer,
        void **ociValue, uint16_t *valueIndicator, void **objectIndicator,
        dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    dpiOracleTypeNum valueOracleTypeNum;
    uint32_t handleType;
    dpiObject *otherObj;
    float floatDiff;
    dpiBytes *bytes;

    // nulls are handled easily
    *objectIndicator = NULL;
    if (data->isNull) {
        *ociValue = NULL;
        *valueIndicator = DPI_OCI_IND_NULL;
        buffer->asRaw = NULL;
        return DPI_SUCCESS;
    }

    // convert all other values
    *valueIndicator = DPI_OCI_IND_NOTNULL;
    valueOracleTypeNum = dataTypeInfo->oracleTypeNum;
    switch (valueOracleTypeNum) {
        case DPI_ORACLE_TYPE_CHAR:
        case DPI_ORACLE_TYPE_NCHAR:
        case DPI_ORACLE_TYPE_VARCHAR:
        case DPI_ORACLE_TYPE_NVARCHAR:
            buffer->asString = NULL;
            if (nativeTypeNum == DPI_NATIVE_TYPE_BYTES) {
                bytes = &data->value.asBytes;
                if (dpiOci__stringAssignText(obj->env, bytes->ptr,
                        bytes->length, &buffer->asString, error) < 0)
                    return DPI_FAILURE;
                *ociValue = buffer->asString;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NATIVE_INT:
        case DPI_ORACLE_TYPE_NUMBER:
            *ociValue = &buffer->asNumber;
            if (nativeTypeNum == DPI_NATIVE_TYPE_INT64)
                return dpiDataBuffer__toOracleNumberFromInteger(&data->value,
                        error, &buffer->asNumber);
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE)
                return dpiDataBuffer__toOracleNumberFromDouble(&data->value,
                        error, &buffer->asNumber);
            if (nativeTypeNum == DPI_NATIVE_TYPE_BYTES)
                return dpiDataBuffer__toOracleNumberFromText(&data->value,
                        obj->env, error, &buffer->asNumber);
            break;
        case DPI_ORACLE_TYPE_NATIVE_FLOAT:
            if (nativeTypeNum == DPI_NATIVE_TYPE_FLOAT) {
                buffer->asFloat = data->value.asFloat;
                *ociValue = &buffer->asFloat;
                return DPI_SUCCESS;
            } else if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE) {
                buffer->asFloat = (float) data->value.asDouble;
                floatDiff = data->value.asDouble - buffer->asFloat;
                if ((floatDiff > 0 && floatDiff > FLT_EPSILON) ||
                        (floatDiff < 0 && -floatDiff > FLT_EPSILON))
                    return dpiError__set(error, "to Oracle value",
                            DPI_ERR_OVERFLOW, "float");
                *ociValue = &buffer->asFloat;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NATIVE_DOUBLE:
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE) {
                buffer->asDouble = data->value.asDouble;
                *ociValue = &buffer->asDouble;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_DATE:
            *ociValue = &buffer->asDate;
            if (nativeTypeNum == DPI_NATIVE_TYPE_TIMESTAMP)
                return dpiDataBuffer__toOracleDate(&data->value,
                        &buffer->asDate);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP:
        case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
        case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
            buffer->asTimestamp = NULL;
            if (nativeTypeNum == DPI_NATIVE_TYPE_TIMESTAMP) {
                if (valueOracleTypeNum == DPI_ORACLE_TYPE_TIMESTAMP)
                    handleType = DPI_OCI_DTYPE_TIMESTAMP;
                else if (valueOracleTypeNum == DPI_ORACLE_TYPE_TIMESTAMP_TZ)
                    handleType = DPI_OCI_DTYPE_TIMESTAMP_TZ;
                else handleType = DPI_OCI_DTYPE_TIMESTAMP_LTZ;
                if (dpiOci__descriptorAlloc(obj->env, &buffer->asTimestamp,
                        handleType, "allocate timestamp", error) < 0)
                    return DPI_FAILURE;
                *ociValue = buffer->asTimestamp;
                return dpiDataBuffer__toOracleTimestamp(&data->value, obj->env,
                        error, buffer->asTimestamp,
                        (valueOracleTypeNum != DPI_ORACLE_TYPE_TIMESTAMP));
            }
            break;
        case DPI_ORACLE_TYPE_OBJECT:
            otherObj = data->value.asObject;
            if (nativeTypeNum == DPI_NATIVE_TYPE_OBJECT) {
                *ociValue = otherObj->instance;
                *objectIndicator = otherObj->indicator;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_BOOLEAN:
            if (nativeTypeNum == DPI_NATIVE_TYPE_BOOLEAN) {
                buffer->asBoolean = data->value.asBoolean;
                *ociValue = &buffer->asBoolean;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_CLOB:
        case DPI_ORACLE_TYPE_NCLOB:
        case DPI_ORACLE_TYPE_BLOB:
        case DPI_ORACLE_TYPE_BFILE:
            if (nativeTypeNum == DPI_NATIVE_TYPE_LOB) {
                buffer->asLobLocator = data->value.asLOB->locator;
                *ociValue = buffer->asLobLocator;
                return DPI_SUCCESS;
            }
            break;

        default:
            break;
    }

    return dpiError__set(error, "to Oracle value",
            DPI_ERR_UNHANDLED_CONVERSION, valueOracleTypeNum, nativeTypeNum);
}


//-----------------------------------------------------------------------------
// dpiObject_addRef() [PUBLIC]
//   Add a reference to the object.
//-----------------------------------------------------------------------------
int dpiObject_addRef(dpiObject *obj)
{
    return dpiGen__addRef(obj, DPI_HTYPE_OBJECT, __func__);
}


//-----------------------------------------------------------------------------
// dpiObject_appendElement() [PUBLIC]
//   Append an element to the collection.
//-----------------------------------------------------------------------------
int dpiObject_appendElement(dpiObject *obj, dpiNativeTypeNum nativeTypeNum,
        dpiData *data)
{
    uint16_t scalarValueIndicator;
    dpiOracleDataBuffer valueBuffer;
    void *indicator;
    dpiError error;
    void *ociValue;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(data)
    if (dpiObject__toOracleValue(obj, &error, &obj->type->elementTypeInfo,
            &valueBuffer, &ociValue, &scalarValueIndicator,
            (void**) &indicator, nativeTypeNum, data) < 0)
        return DPI_FAILURE;
    if (!indicator)
        indicator = &scalarValueIndicator;
    status = dpiOci__collAppend(obj->type->conn, ociValue, indicator,
            obj->instance, &error);
    dpiObject__clearOracleValue(obj->env, &error, &valueBuffer,
            obj->type->elementTypeInfo.oracleTypeNum);
    return status;
}


//-----------------------------------------------------------------------------
// dpiObject_copy() [PUBLIC]
//   Create a copy of the object and return it. Return NULL upon error.
//-----------------------------------------------------------------------------
int dpiObject_copy(dpiObject *obj, dpiObject **copiedObj)
{
    dpiObject *tempObj;
    dpiError error;

    if (dpiGen__startPublicFn(obj, DPI_HTYPE_OBJECT, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(copiedObj)
    if (dpiObjectType_createObject(obj->type, &tempObj) < 0)
        return DPI_FAILURE;
    if (dpiOci__objectCopy(obj, tempObj, &error) < 0) {
        dpiObject__free(tempObj, &error);
        return DPI_FAILURE;
    }
    *copiedObj = tempObj;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObject_deleteElementByIndex() [PUBLIC]
//   Delete the element at the specified index in the collection.
//-----------------------------------------------------------------------------
int dpiObject_deleteElementByIndex(dpiObject *obj, int32_t index)
{
    dpiError error;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    return dpiOci__tableDelete(obj, index, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getAttributeValue() [PUBLIC]
//   Get the value of the given attribute from the object.
//-----------------------------------------------------------------------------
int dpiObject_getAttributeValue(dpiObject *obj, dpiObjectAttr *attr,
        dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    int16_t scalarValueIndicator;
    void *valueIndicator, *tdo;
    dpiOracleData value;
    dpiError error;

    // validate parameters
    if (dpiGen__startPublicFn(obj, DPI_HTYPE_OBJECT, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(data)
    if (dpiGen__checkHandle(attr, DPI_HTYPE_OBJECT_ATTR, "get attribute value",
            &error) < 0)
        return DPI_FAILURE;
    if (attr->belongsToType->tdo != obj->type->tdo)
        return dpiError__set(&error, "get attribute value", DPI_ERR_WRONG_ATTR,
                attr->nameLength, attr->name, obj->type->schemaLength,
                obj->type->schema, obj->type->nameLength, obj->type->name);

    // get attribute value
    if (dpiOci__objectGetAttr(obj, attr, &scalarValueIndicator,
            &valueIndicator, &value.asRaw, &tdo, &error) < 0)
        return DPI_FAILURE;

    // determine the proper null indicator
    if (!valueIndicator)
        valueIndicator = &scalarValueIndicator;

    // check to see if type is supported
    if (!attr->typeInfo.oracleTypeNum)
        return dpiError__set(&error, "get attribute value",
                DPI_ERR_UNHANDLED_DATA_TYPE, attr->typeInfo.ociTypeCode);

    // convert to output data format
    return dpiObject__fromOracleValue(obj, &error, &attr->typeInfo, &value,
            valueIndicator, nativeTypeNum, data);
}


//-----------------------------------------------------------------------------
// dpiObject_getElementExistsByIndex() [PUBLIC]
//   Return boolean indicating if an element exists in the collection at the
// specified index.
//-----------------------------------------------------------------------------
int dpiObject_getElementExistsByIndex(dpiObject *obj, int32_t index,
        int *exists)
{
    dpiError error;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(exists)
    return dpiOci__tableExists(obj, index, exists, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getElementValueByIndex() [PUBLIC]
//   Return the element at the given index in the collection.
//-----------------------------------------------------------------------------
int dpiObject_getElementValueByIndex(dpiObject *obj, int32_t index,
        dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    dpiOracleData value;
    void *indicator;
    dpiError error;
    int exists;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(data)
    if (dpiOci__collGetElem(obj->type->conn, obj->instance, index, &exists,
            &value.asRaw, &indicator, &error) < 0)
        return DPI_FAILURE;
    if (!exists)
        return dpiError__set(&error, "get element value",
                DPI_ERR_INVALID_INDEX, index);
    return dpiObject__fromOracleValue(obj, &error, &obj->type->elementTypeInfo,
            &value, indicator, nativeTypeNum, data);
}


//-----------------------------------------------------------------------------
// dpiObject_getFirstIndex() [PUBLIC]
//   Return the index of the first entry in the collection.
//-----------------------------------------------------------------------------
int dpiObject_getFirstIndex(dpiObject *obj, int32_t *index, int *exists)
{
    dpiError error;
    int32_t size;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(index)
    DPI_CHECK_PTR_NOT_NULL(exists)
    if (dpiOci__tableSize(obj, &size, &error) < 0)
        return DPI_FAILURE;
    *exists = (size != 0);
    if (*exists)
        return dpiOci__tableFirst(obj, index, &error);
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObject_getLastIndex() [PUBLIC]
//   Return the index of the last entry in the collection.
//-----------------------------------------------------------------------------
int dpiObject_getLastIndex(dpiObject *obj, int32_t *index, int *exists)
{
    dpiError error;
    int32_t size;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(index)
    DPI_CHECK_PTR_NOT_NULL(exists)
    if (dpiOci__tableSize(obj, &size, &error) < 0)
        return DPI_FAILURE;
    *exists = (size != 0);
    if (*exists)
        return dpiOci__tableLast(obj, index, &error);
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObject_getNextIndex() [PUBLIC]
//   Return the index of the next entry in the collection following the index
// specified. If there is no next entry, exists is set to 0.
//-----------------------------------------------------------------------------
int dpiObject_getNextIndex(dpiObject *obj, int32_t index, int32_t *nextIndex,
        int *exists)
{
    dpiError error;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(nextIndex)
    DPI_CHECK_PTR_NOT_NULL(exists)
    return dpiOci__tableNext(obj, index, nextIndex, exists, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getPrevIndex() [PUBLIC]
//   Return the index of the previous entry in the collection preceding the
// index specified. If there is no previous entry, exists is set to 0.
//-----------------------------------------------------------------------------
int dpiObject_getPrevIndex(dpiObject *obj, int32_t index, int32_t *prevIndex,
        int *exists)
{
    dpiError error;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(prevIndex)
    DPI_CHECK_PTR_NOT_NULL(exists)
    return dpiOci__tablePrev(obj, index, prevIndex, exists, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getSize() [PUBLIC]
//   Return the size of the collection.
//-----------------------------------------------------------------------------
int dpiObject_getSize(dpiObject *obj, int32_t *size)
{
    dpiError error;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(size)
    return dpiOci__collSize(obj->type->conn, obj->instance, size, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_release() [PUBLIC]
//   Release a reference to the object.
//-----------------------------------------------------------------------------
int dpiObject_release(dpiObject *obj)
{
    return dpiGen__release(obj, DPI_HTYPE_OBJECT, __func__);
}


//-----------------------------------------------------------------------------
// dpiObject_setAttributeValue() [PUBLIC]
//   Create a copy of the object and return it. Return NULL upon error.
//-----------------------------------------------------------------------------
int dpiObject_setAttributeValue(dpiObject *obj, dpiObjectAttr *attr,
        dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    void *valueIndicator, *ociValue;
    dpiOracleDataBuffer valueBuffer;
    uint16_t scalarValueIndicator;
    dpiError error;
    int status;

    // validate parameters
    if (dpiGen__startPublicFn(obj, DPI_HTYPE_OBJECT, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(data)
    if (dpiGen__checkHandle(attr, DPI_HTYPE_OBJECT_ATTR, "set attribute value",
            &error) < 0)
        return DPI_FAILURE;
    if (attr->belongsToType->tdo != obj->type->tdo)
        return dpiError__set(&error, "set attribute value", DPI_ERR_WRONG_ATTR,
                attr->nameLength, attr->name, obj->type->schemaLength,
                obj->type->schema, obj->type->nameLength, obj->type->name);

    // check to see if type is supported
    if (!attr->typeInfo.oracleTypeNum)
        return dpiError__set(&error, "get attribute value",
                DPI_ERR_UNHANDLED_DATA_TYPE, attr->typeInfo.ociTypeCode);

    // convert to input data format
    if (dpiObject__toOracleValue(obj, &error, &attr->typeInfo, &valueBuffer,
            &ociValue, &scalarValueIndicator, &valueIndicator, nativeTypeNum,
            data) < 0)
        return DPI_FAILURE;

    // set attribute value
    status = dpiOci__objectSetAttr(obj, attr, scalarValueIndicator,
            valueIndicator, ociValue, &error);
    dpiObject__clearOracleValue(obj->env, &error, &valueBuffer,
            attr->typeInfo.oracleTypeNum);
    return status;
}


//-----------------------------------------------------------------------------
// dpiObject_setElementValueByIndex() [PUBLIC]
//   Set the element at the specified index to the given value.
//-----------------------------------------------------------------------------
int dpiObject_setElementValueByIndex(dpiObject *obj, int32_t index,
        dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    dpiOracleDataBuffer valueBuffer;
    uint16_t scalarValueIndicator;
    void *indicator;
    dpiError error;
    void *ociValue;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(data)
    if (dpiObject__toOracleValue(obj, &error, &obj->type->elementTypeInfo,
            &valueBuffer, &ociValue, &scalarValueIndicator,
            (void**) &indicator, nativeTypeNum, data) < 0)
        return DPI_FAILURE;
    if (!indicator)
        indicator = &scalarValueIndicator;
    status = dpiOci__collAssignElem(obj->type->conn, index, ociValue,
            indicator, obj->instance, &error);
    dpiObject__clearOracleValue(obj->env, &error, &valueBuffer,
            obj->type->elementTypeInfo.oracleTypeNum);
    return status;
}


//-----------------------------------------------------------------------------
// dpiObject_trim() [PUBLIC]
//   Trim a number of elements from the end of the collection.
//-----------------------------------------------------------------------------
int dpiObject_trim(dpiObject *obj, uint32_t numToTrim)
{
    dpiError error;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    return dpiOci__collTrim(obj->type->conn, numToTrim, obj->instance, &error);
}

