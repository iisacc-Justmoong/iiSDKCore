#include <Python.h>

static PyObject* echo(PyObject*, PyObject* input) {
    Py_INCREF(input);
    return input;
}

static PyObject* fail(PyObject*, PyObject*) {
    PyErr_SetString(PyExc_ValueError, "fixture failure");
    return nullptr;
}

static PyObject* cycle(PyObject*, PyObject*) {
    PyObject* list = PyList_New(1);
    if (!list) return nullptr;
    Py_INCREF(list);
    PyList_SET_ITEM(list, 0, list);
    return list;
}

static PyObject* huge(PyObject*, PyObject*) {
    char text[] = "18446744073709551616";
    return PyLong_FromString(text, nullptr, 10);
}

static PyObject* rewrite_origin(PyObject*, PyObject*) {
    PyObject* module = PyDict_GetItemString(PyImport_GetModuleDict(), "iisdk_cpython_fixture");
    PyObject* spec = module ? PyObject_GetAttrString(module, "__spec__") : nullptr;
    PyObject* origin = PyUnicode_FromString("fixture.py");
    if (!spec || !origin || PyObject_SetAttrString(spec, "origin", origin) < 0) {
        Py_XDECREF(origin);
        Py_XDECREF(spec);
        return nullptr;
    }
    Py_DECREF(origin);
    Py_DECREF(spec);
    Py_RETURN_NONE;
}

static PyMethodDef methods[] = {
    {"echo", echo, METH_O, nullptr},
    {"fail", fail, METH_NOARGS, nullptr},
    {"cycle", cycle, METH_NOARGS, nullptr},
    {"huge", huge, METH_NOARGS, nullptr},
    {"rewrite_origin", rewrite_origin, METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr}
};
static PyModuleDef definition = {PyModuleDef_HEAD_INIT, "iisdk_cpython_fixture", nullptr, -1, methods};
PyMODINIT_FUNC PyInit_iisdk_cpython_fixture() { return PyModule_Create(&definition); }
