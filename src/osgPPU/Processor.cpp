/***************************************************************************
 *   Copyright (c) 2008   Art Tevs                                         *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Lesser General Public License as        *
 *   published by the Free Software Foundation; either version 3 of        *
 *   the License, or (at your option) any later version.                   *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Lesse General Public License for more details.                    *
 *                                                                         *
 *   The full license is in LICENSE file included with this distribution.  *
 ***************************************************************************/

#include <osgPPU/Processor.h>
#include <osgPPU/Visitor.h>
#include <osg/Texture2D>
#include <osg/Depth>
#include <osg/Notify>
#include <osg/ClampColor>
#include <osg/FrameBufferObject>
#include <osg/BlendColor>
#include <osg/BlendEquation>
#include <osg/Material>

#include <assert.h>

#include <osgUtil/RenderBin>

namespace osgPPU
{

//------------------------------------------------------------------------------
// Helper class used as render bin
//------------------------------------------------------------------------------
class PPUProcessingBin : public osgUtil::RenderBin
{
    public:
        PPUProcessingBin(const std::string& name) : osgUtil::RenderBin()
        {
            setName(name);
        }
};

// This is a default rendering bin which all units are usign
static osg::ref_ptr<osgUtil::RenderBin> DefaultBin = new PPUProcessingBin("PPUProcessingBin");

// just an instance for faster access
static osg::ref_ptr<UnitVisitor> sCleanUpdateTraverseMaskVisitor = new CleanUpdateTraversedVisitor;
static osg::ref_ptr<UnitVisitor> sCleanCullTraverseMaskVisitor = new CleanCullTraversedVisitor;

//------------------------------------------------------------------------------
Processor::Processor()
{
    // set some variables
    mbDirty = true;
    mbDirtyUnitGraph = true;
    mUseColorClamp = true;

    // first we have to create a render bin which will hold the units
    // of the subgraph.
    if (!osgUtil::RenderBin::getRenderBinPrototype(DefaultBin->getName()))
    {
        osgUtil::RenderBin::addRenderBinPrototype(DefaultBin->getName(), DefaultBin.get());
    }

    // no culling
    setCullingActive(false);
}

//------------------------------------------------------------------------------
Processor::Processor(const Processor& pp, const osg::CopyOp& copyop) :
    osg::Group(pp, copyop),
    mCamera(pp.mCamera),
    //mVisitor(pp.mVisitor),
    mbDirty(pp.mbDirty),
    mbDirtyUnitGraph(pp.mbDirtyUnitGraph),
    mUseColorClamp(pp.mUseColorClamp)
{
}

//------------------------------------------------------------------------------
Processor::~Processor()
{
}

//------------------------------------------------------------------------------
void Processor::init()
{
    // create default state for post processing effects
    osg::StateSet* mStateSet = getOrCreateStateSet();
    mStateSet->clear();

    // the processor's stateset have to be activated as first in the pipeline
    mStateSet->setRenderBinDetails(100, DefaultBin->getName());
    //mStateSet->setBinNumber(100);

    // setup default state set modes
    mStateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    mStateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    mStateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

    // setup default blending functions
    mStateSet->setMode(GL_BLEND, osg::StateAttribute::OFF);
    mStateSet->setAttribute(new osg::BlendFunc(), osg::StateAttribute::ON);
    mStateSet->setAttribute(new osg::BlendColor(osg::Vec4(1,1,1,1)), osg::StateAttribute::ON);
    mStateSet->setAttribute(new osg::BlendEquation(osg::BlendEquation::FUNC_ADD), osg::StateAttribute::ON);

    // we shouldn't write to the depth buffer
    osg::Depth* ds = new osg::Depth();
    ds->setWriteMask(false);
    mStateSet->setAttribute(ds, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

    // the processor is of fixed function pipeline, however the childs (units) might not
    mStateSet->setAttribute(new osg::Program(), osg::StateAttribute::ON);
    mStateSet->setAttribute(new osg::FrameBufferObject(), osg::StateAttribute::ON);
    mStateSet->setAttribute(new osg::Material(), osg::StateAttribute::ON);

    // disable color clamping, because we want to work on real hdr values
    if (mUseColorClamp)
    {
        osg::ClampColor* clamp = new osg::ClampColor();
        clamp->setClampVertexColor(GL_FALSE);
        clamp->setClampFragmentColor(GL_FALSE);
        clamp->setClampReadColor(GL_FALSE);
        mStateSet->setAttribute(clamp, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
    }

    // not dirty anymore
    mbDirty = false;
}


//------------------------------------------------------------------------------
void Processor::setCamera(osg::Camera* camera)
{
    // dirty subgraph if camera changed
    if (mCamera.get() != camera)
        dirtyUnitSubgraph();

    // setup camera
    mCamera = camera;
}

//------------------------------------------------------------------------------
Unit* Processor::findUnit(const std::string& name)
{
    if (!mbDirtyUnitGraph)
    {
        FindUnitVisitor uv(name);
        uv.run(this);
        return uv.getResult();
        //return mVisitor->findUnit(name, this);
    }
    return NULL;
}

//------------------------------------------------------------------------------
bool Processor::removeUnit(Unit* unit)
{
    if (mbDirtyUnitGraph)
    {
        osg::notify(osg::INFO) << "osgPPU::Processor::removeUnit(" << unit->getName() << ") - cannot remove unit because the graph is not valid. " << std::endl;
        return false;
    }

    RemoveUnitVisitor uv;
    uv.run(unit);

    return true;
}


//------------------------------------------------------------------------------
void Processor::traverse(osg::NodeVisitor& nv)
{
    // if not initialized before, then do it
    if (mbDirty) init();

    // if processor is propagated by the osg's visitor, then
    if (nv.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
    {
        if (mbDirtyUnitGraph)
        {
            mbDirtyUnitGraph = false;

            // first resolve all cycles in the set
            ResolveUnitsCyclesVisitor rv;
            rv.run(this);

            // debug information
            osg::notify(osg::INFO) << "--------------------------------------------------------------------" << std::endl;
            osg::notify(osg::INFO) << "BEGIN " << getName() << std::endl;

            SetupUnitRenderingVisitor sv(this);
            sv.run(this);

            osg::notify(osg::INFO) << "END " << getName() << std::endl;
            osg::notify(osg::INFO) << "--------------------------------------------------------------------" << std::endl;

            // optimize subgraph
            OptimizeUnitsVisitor ov;
            ov.run(this);
        }

        // first we need to clear traversion bit of every unit
        sCleanUpdateTraverseMaskVisitor->run(this);
    }

    // if processor is propagated by a cull visitor, hence first clean the traverse bit
    if (nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
    {
        // first we need to clear traversion bit of every unit
        sCleanCullTraverseMaskVisitor->run(this);
    }

    // just a normal traversion
    if (mbDirtyUnitGraph == false || nv.getVisitorType() == osg::NodeVisitor::NODE_VISITOR)
    {
        osg::Group::traverse(nv);
    }


}


}; //end namespace




