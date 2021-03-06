/*
 * opencog/atoms/base/ClassServer.cc
 *
 * Copyright (C) 2002-2007 Novamente LLC
 * Copyright (C) 2008 by OpenCog Foundation
 * Copyright (C) 2009,2017 Linas Vepstas <linasvepstas@gmail.com>
 * All Rights Reserved
 *
 * Written by Thiago Maia <thiago@vettatech.com>
 *            Andre Senna <senna@vettalabs.com>
 *            Gustavo Gama <gama@vettalabs.com>
 *            Linas Vepstas <linasvepstas@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "ClassServer.h"

#include <exception>

#include <opencog/atoms/base/types.h>
#include <opencog/atoms/base/atom_types.h>
#include <opencog/atoms/base/Atom.h>
#include <opencog/atoms/base/ProtoAtom.h>

//#define DPRINTF printf
#define DPRINTF(...)

using namespace opencog;

ClassServer::ClassServer(void)
{
	nTypes = 0;
	_maxDepth = 0;
	_is_init = false;
}

static int tmod = 0;

void ClassServer::beginTypeDecls(void)
{
	tmod++;
	_is_init = false;
}

void ClassServer::endTypeDecls(void)
{
	// Valid types are odd-numbered.
	tmod++;
}

Type ClassServer::declType(const Type parent, const std::string& name)
{
    if (1 != tmod%2)
        throw InvalidParamException(TRACE_INFO,
            "Improper type declararition; must call beginTypeDecls() first\n");

    // Check if a type with this name already exists. If it does, then
    // the second and subsequent calls are to be interpreted as defining
    // multiple inheritance for this type.  A real-life example is the
    // GroundedSchemeNode, which inherits from several types.
    Type type = getType(name);
    if (type != NOTYPE) {
        std::lock_guard<std::mutex> l(type_mutex);

        // ... unless someone is accidentally declaring the same type
        // in some different place. In that case, we throw an error.
        if (_mod[type] != tmod)
            throw InvalidParamException(TRACE_INFO,
                "Type \"%s\" has already been declared (%d)\n",
                name.c_str(), type);

        inheritanceMap[parent][type] = true;

        Type maxd = 1;
        setParentRecursively(parent, type, maxd);
        if (_maxDepth < maxd) _maxDepth = maxd;
        return type;
    }

    std::unique_lock<std::mutex> l(type_mutex);
    // Assign type code and increment type counter.
    type = nTypes++;

    // Resize inheritanceMap container.
    inheritanceMap.resize(nTypes);
    recursiveMap.resize(nTypes);
    _code2NameMap.resize(nTypes);
    _mod.resize(nTypes);
    _atomFactory.resize(nTypes);
    _validator.resize(nTypes);

    for (auto& bv: inheritanceMap) bv.resize(nTypes, false);
    for (auto& bv: recursiveMap) bv.resize(nTypes, false);

    inheritanceMap[type][type]   = true;
    inheritanceMap[parent][type] = true;
    recursiveMap[type][type]     = true;
    name2CodeMap[name]           = type;
    _code2NameMap[type]          = &(name2CodeMap.find(name)->first);
    _mod[type]                   = tmod;

    Type maxd = 1;
    setParentRecursively(parent, type, maxd);
    if (_maxDepth < maxd) _maxDepth = maxd;

    // unlock mutex before sending signal which could call
    l.unlock();

    // Emit add type signal.
    _addTypeSignal.emit(type);

    return type;
}

void ClassServer::setParentRecursively(Type parent, Type type, Type& maxd)
{
    if (recursiveMap[parent][type]) return;

    bool incr = false;
    recursiveMap[parent][type] = true;
    for (Type i = 0; i < nTypes; ++i) {
        if ((recursiveMap[i][parent]) and (i != parent)) {
            incr = true;
            setParentRecursively(i, type, maxd);
        }
    }
    if (incr) maxd++;
}

TypeSignal& ClassServer::typeAddedSignal()
{
    return _addTypeSignal;
}

void ClassServer::addFactory(Type t, AtomFactory* fact)
{
	std::unique_lock<std::mutex> l(type_mutex);
	_atomFactory[t] = fact;
	_is_init = false;
}

Handle validating_factory(const Handle& atom_to_check)
{
	ClassServer::Validator* checker =
		classserver().getValidator(atom_to_check->get_type());

	/* Well, is it OK, or not? */
	if (not checker(atom_to_check))
		throw SyntaxException(TRACE_INFO,
		     "Invalid Atom syntax: %s",
		     atom_to_check->to_string().c_str());

	return atom_to_check;
}

void ClassServer::addValidator(Type t, Validator* checker)
{
	std::unique_lock<std::mutex> l(type_mutex);
	_validator[t] = checker;
	if (not _atomFactory[t])
		_atomFactory[t] = validating_factory;
	_is_init = false;
}

// Perform a depth-first recursive search for a factory,
// up to a maximum depth.
template<typename RTN_TYPE>
RTN_TYPE* ClassServer::searchToDepth(const std::vector<RTN_TYPE*>& vect,
                                     Type t, int depth) const
{
	// If there is a factory, then return it.
	RTN_TYPE* fpr = vect[t];
	if (fpr) return fpr;

	// Perhaps one of the parent types has a factory.
	// Perform a depth-first recursion.
	depth--;
	if (depth < 0) return nullptr;

	std::vector<Type> parents;
	getParents(t, back_inserter(parents));
	for (auto p: parents)
	{
		RTN_TYPE* fact = searchToDepth<RTN_TYPE>(vect, p, depth);
		if (fact) return fact;
	}

	return nullptr;
}

template<typename RTN_TYPE>
RTN_TYPE* ClassServer::getOper(const std::vector<RTN_TYPE*>& vect,
                               Type t) const
{
	if (nTypes <= t) return nullptr;

	// If there is a factory, then return it.
	RTN_TYPE* fpr = vect[t];
	if (fpr) return fpr;

	// Perhaps one of the parent types has a factory.
	//
	// We want to use a breadth-first recursion, and not
	// the simpler-to-code depth-first recursion.  That is,
	// we do NOT want some deep parent factory, when there
	// is some factory at a shallower level.
	//
	for (int search_depth = 1; search_depth <= _maxDepth; search_depth++)
	{
		RTN_TYPE* fact = searchToDepth<RTN_TYPE>(vect, t, search_depth);
		if (fact) return fact;
	}

	return nullptr;
}

ClassServer::AtomFactory* ClassServer::getFactory(Type t) const
{
	if (not _is_init) init();
	return _atomFactory[t];
}

ClassServer::Validator* ClassServer::getValidator(Type t) const
{
	if (not _is_init) init();
	return _validator[t];
}

void ClassServer::init() const
{
	// The goal here is to cache the various factories that child
	// nodes might run. The getOper<AtomFactory>() is too expensive
	// to run for every node creation. If damages performance by
	// 10x per Node creation, and 5x for every link creation.
	//
	// Unfortunately, creating this cache is tricky, because the
	// factories get added in random order, can can clobber
	// one-another, so we have to wait to do this until after
	// all factories have been declared.
	for (Type parent = 0; parent < nTypes; parent++)
	{
		if (_atomFactory[parent])
		{
			for (Type chi = parent+1; chi < nTypes; ++chi)
			{
				if (recursiveMap[parent][chi] and
				    nullptr == _atomFactory[chi])
				{
					_atomFactory[chi] = getOper<AtomFactory>(_atomFactory, chi);
				}
			}
		}

		if (_validator[parent])
		{
			for (Type chi = parent+1; chi < nTypes; ++chi)
			{
				if (recursiveMap[parent][chi] and
				    nullptr == _validator[chi])
				{
					_validator[chi] = getOper<Validator>(_validator, chi);
				}
			}
		}
	}
	_is_init = true;
}

Handle ClassServer::factory(const Handle& h) const
{
	// If there is a factory, then use it.
	AtomFactory* fact = getFactory(h->get_type());
	if (fact)
		return (*fact)(h);

	return h;
}

Type ClassServer::getNumberOfClasses() const
{
    return nTypes;
}

bool ClassServer::isA_non_recursive(Type type, Type parent) const
{
    std::lock_guard<std::mutex> l(type_mutex);
    if ((type >= nTypes) || (parent >= nTypes)) return false;
    return inheritanceMap[parent][type];
}

bool ClassServer::isDefined(const std::string& typeName) const
{
    std::lock_guard<std::mutex> l(type_mutex);
    return name2CodeMap.find(typeName) != name2CodeMap.end();
}

Type ClassServer::getType(const std::string& typeName) const
{
    std::lock_guard<std::mutex> l(type_mutex);
    std::unordered_map<std::string, Type>::const_iterator it = name2CodeMap.find(typeName);
    if (it == name2CodeMap.end()) {
        return NOTYPE;
    }
    return it->second;
}

const std::string& ClassServer::getTypeName(Type type) const
{
    static std::string nullString = "*** Unknown Type! ***";

    if (nTypes <= type) return nullString;

    std::lock_guard<std::mutex> l(type_mutex);
    const std::string* name = _code2NameMap[type];
    if (name) return *name;
    return nullString;
}

ClassServer& opencog::classserver()
{
    static std::unique_ptr<ClassServer> instance(new ClassServer());
    return *instance;
}
