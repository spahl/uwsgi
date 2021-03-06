#include "uwsgi_python.h"

extern struct uwsgi_server uwsgi;
extern struct uwsgi_python up;
extern PyTypeObject uwsgi_InputType;

void *uwsgi_request_subhandler_web3(struct wsgi_request *wsgi_req, struct uwsgi_app *wi) {

	PyObject *zero;

	int i;
        PyObject *pydictkey, *pydictvalue;
        char *path_info;

        for (i = 0; i < wsgi_req->var_cnt; i += 2) {
#ifdef UWSGI_DEBUG
                uwsgi_debug("%.*s: %.*s\n", wsgi_req->hvec[i].iov_len, wsgi_req->hvec[i].iov_base, wsgi_req->hvec[i+1].iov_len, wsgi_req->hvec[i+1].iov_base);
#endif
#ifdef PYTHREE
                pydictkey = PyUnicode_DecodeLatin1(wsgi_req->hvec[i].iov_base, wsgi_req->hvec[i].iov_len, NULL);
                pydictvalue = PyUnicode_DecodeLatin1(wsgi_req->hvec[i + 1].iov_base, wsgi_req->hvec[i + 1].iov_len, NULL);
#else
                pydictkey = PyString_FromStringAndSize(wsgi_req->hvec[i].iov_base, wsgi_req->hvec[i].iov_len);
                pydictvalue = PyString_FromStringAndSize(wsgi_req->hvec[i + 1].iov_base, wsgi_req->hvec[i + 1].iov_len);
#endif
                PyDict_SetItem(wsgi_req->async_environ, pydictkey, pydictvalue);
                Py_DECREF(pydictkey);
                Py_DECREF(pydictvalue);
        }

        if (wsgi_req->uh.modifier1 == UWSGI_MODIFIER_MANAGE_PATH_INFO) {
                wsgi_req->uh.modifier1 = 0;
                pydictkey = PyDict_GetItemString(wsgi_req->async_environ, "SCRIPT_NAME");
                if (pydictkey) {
                        if (PyString_Check(pydictkey)) {
                                pydictvalue = PyDict_GetItemString(wsgi_req->async_environ, "PATH_INFO");
                                if (pydictvalue) {
                                        if (PyString_Check(pydictvalue)) {
                                                path_info = PyString_AsString(pydictvalue);
                                                PyDict_SetItemString(wsgi_req->async_environ, "PATH_INFO", PyString_FromString(path_info + PyString_Size(pydictkey)));
                                        }
                                }
                        }
                }
        }

        // if async_post is mapped as a file, directly use it as wsgi.input
        if (wsgi_req->async_post) {
#ifdef PYTHREE
                wsgi_req->async_input = PyFile_FromFd(fileno(wsgi_req->async_post), "web3_input", "rb", 0, NULL, NULL, NULL, 0);
#else
                wsgi_req->async_input = PyFile_FromFile(wsgi_req->async_post, "web3_input", "r", NULL);
#endif
        }
        else {
                // create wsgi.input custom object
                wsgi_req->async_input = (PyObject *) PyObject_New(uwsgi_Input, &uwsgi_InputType);
                ((uwsgi_Input*)wsgi_req->async_input)->wsgi_req = wsgi_req;
                ((uwsgi_Input*)wsgi_req->async_input)->pos = 0;
                ((uwsgi_Input*)wsgi_req->async_input)->readline_pos = 0;
                ((uwsgi_Input*)wsgi_req->async_input)->readline_max_size = 0;

        }

        PyDict_SetItemString(wsgi_req->async_environ, "web3.input", wsgi_req->async_input);

	PyDict_SetItemString(wsgi_req->async_environ, "web3.version", wi->gateway_version);

	zero = PyFile_FromFile(stderr, "web3_input", "w", NULL);
	PyDict_SetItemString(wsgi_req->async_environ, "web3.errors", zero);
	Py_DECREF(zero);

	PyDict_SetItemString(wsgi_req->async_environ, "web3.run_once", Py_False);

	PyDict_SetItemString(wsgi_req->async_environ, "web3.multithread", Py_False);
	if (uwsgi.numproc == 1) {
		PyDict_SetItemString(wsgi_req->async_environ, "web3.multiprocess", Py_False);
	}
	else {
		PyDict_SetItemString(wsgi_req->async_environ, "web3.multiprocess", Py_True);
	}

	if (wsgi_req->scheme_len > 0) {
		zero = UWSGI_PYFROMSTRINGSIZE(wsgi_req->scheme, wsgi_req->scheme_len);
	}
	else if (wsgi_req->https_len > 0) {
		if (!strncasecmp(wsgi_req->https, "on", 2) || wsgi_req->https[0] == '1') {
			zero = UWSGI_PYFROMSTRING("https");
		}
		else {
			zero = UWSGI_PYFROMSTRING("http");
		}
	}
	else {
		zero = UWSGI_PYFROMSTRING("http");
	}
	PyDict_SetItemString(wsgi_req->async_environ, "web3.url_scheme", zero);
	Py_DECREF(zero);


	wsgi_req->async_app = wi->callable;

	// export .env only in non-threaded mode
        if (uwsgi.threads < 2) {
        	PyDict_SetItemString(up.embedded_dict, "env", wsgi_req->async_environ);
        }

        PyDict_SetItemString(wsgi_req->async_environ, "uwsgi.version", wi->uwsgi_version);

        if (uwsgi.cores > 1) {
                PyDict_SetItemString(wsgi_req->async_environ, "uwsgi.core", PyInt_FromLong(wsgi_req->async_id));
        }

        // cache this ?
        if (uwsgi.cluster_fd >= 0) {
                zero = PyString_FromString(uwsgi.cluster);
                PyDict_SetItemString(wsgi_req->async_environ, "uwsgi.cluster", zero);
                Py_DECREF(zero);
                zero = PyString_FromString(uwsgi.hostname);
                PyDict_SetItemString(wsgi_req->async_environ, "uwsgi.cluster_node", zero);
                Py_DECREF(zero);
        }

        PyDict_SetItemString(wsgi_req->async_environ, "uwsgi.node", wi->uwsgi_node);


	// call

	PyTuple_SetItem(wsgi_req->async_args, 0, wsgi_req->async_environ);
	return python_call(wsgi_req->async_app, wsgi_req->async_args, uwsgi.catch_exceptions, wsgi_req);
}


int uwsgi_response_subhandler_web3(struct wsgi_request *wsgi_req) {

	PyObject *pychunk;
	ssize_t wsize;

	// ok its a yield
	if (!wsgi_req->async_placeholder) {
		if (PyTuple_Check((PyObject *)wsgi_req->async_result)) {
			if (PyTuple_Size((PyObject *)wsgi_req->async_result) != 3) { 
				uwsgi_log("invalid Web3 response.\n"); 
				goto clear; 
			} 



			wsgi_req->async_placeholder = PyTuple_GetItem((PyObject *)wsgi_req->async_result, 0); 
			Py_INCREF((PyObject *)wsgi_req->async_placeholder);

			PyObject *spit_args = PyTuple_New(2);

			PyObject *status = PyTuple_GetItem((PyObject *)wsgi_req->async_result, 1);
			Py_INCREF(status);
			PyTuple_SetItem(spit_args, 0, status);

			PyObject *headers = PyTuple_GetItem((PyObject *)wsgi_req->async_result, 2);
			Py_INCREF(headers);
			PyTuple_SetItem(spit_args, 1, headers);

			if (py_uwsgi_spit(NULL, spit_args) == Py_None) { 
				Py_DECREF(spit_args);
				goto clear; 
			} 


			Py_DECREF(spit_args);

			if (PyString_Check((PyObject *)wsgi_req->async_placeholder)) {
                		if ((wsize = wsgi_req->socket->proto_write(wsgi_req, PyString_AsString(wsgi_req->async_placeholder), PyString_Size(wsgi_req->async_placeholder))) < 0) {
                        		uwsgi_error("write()");
                        		goto clear;
                		}
                		wsgi_req->response_size += wsize;
                		goto clear;
        		}

			PyObject *tmp = (PyObject *)wsgi_req->async_placeholder;

			wsgi_req->async_placeholder = PyObject_GetIter( (PyObject *)wsgi_req->async_placeholder );

			Py_DECREF(tmp);

			if (!wsgi_req->async_placeholder) {
				goto clear;
			}
#ifdef UWSGI_ASYNC
			if (uwsgi.async > 1) {
				return UWSGI_AGAIN;
			}
#endif
		}
		else {
			uwsgi_log("invalid Web3 response.\n"); 
			goto clear; 
		}
	}



	pychunk = PyIter_Next(wsgi_req->async_placeholder);

	if (!pychunk) {
		if (PyErr_Occurred()) PyErr_Print();
		goto clear;
	}


	if (PyString_Check(pychunk)) {
		if ((wsize = wsgi_req->socket->proto_write(wsgi_req,  PyString_AsString(pychunk), PyString_Size(pychunk))) < 0) {
			uwsgi_error("write()");
			Py_DECREF(pychunk);
			goto clear;
		}
		wsgi_req->response_size += wsize;
	}


	Py_DECREF(pychunk);
	return UWSGI_AGAIN;

clear:
	Py_XDECREF((PyObject *)wsgi_req->async_placeholder);

	Py_DECREF((PyObject *)wsgi_req->async_result);
	PyErr_Clear();

	return UWSGI_OK;
}

