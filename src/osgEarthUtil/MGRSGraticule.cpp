/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2016 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarthUtil/MGRSGraticule>
#include <osgEarthUtil/MGRSFormatter>

#include <osgEarthFeatures/GeometryCompiler>
#include <osgEarthFeatures/TextSymbolizer>

#include <osgEarth/ECEF>
#include <osgEarth/Registry>
#include <osgEarth/CullingUtils>
#include <osgEarth/Utils>

#include <osg/BlendFunc>
#include <osg/PagedLOD>
#include <osg/Depth>
#include <osg/LogicOp>
#include <osg/MatrixTransform>
#include <osg/ClipNode>
#include <osgDB/FileNameUtils>
#include <osgDB/ReaderWriter>


#define LC "[MGRSGraticule] "

using namespace osgEarth;
using namespace osgEarth::Util;
using namespace osgEarth::Features;
using namespace osgEarth::Symbology;

#define MGRS_GRATICULE_EXTENSION "osgearthutil_mgrs_graticule"

#define MGRS_GRATICULE_OBJECT_NAME "osgEarth.Util.MGRSGraticule"

//---------------------------------------------------------------------------


MGRSGraticule::MGRSGraticule(MapNode* mapNode) :
_mapNode(mapNode)
{
    ctor();
    rebuild();
}

MGRSGraticule::MGRSGraticule(MapNode* mapNode, const MGRSGraticuleOptions& options) :
MGRSGraticuleOptions(options),
_mapNode(mapNode)
{
    ctor();
    rebuild();
}

void
MGRSGraticule::refresh()
{
    rebuild();
}

void
MGRSGraticule::ctor()
{
    setName(MGRS_GRATICULE_OBJECT_NAME);

    osg::StateSet* ss = this->getOrCreateStateSet();

    // make the shared depth attr:
    ss->setMode( GL_DEPTH_TEST, 0 );
    ss->setMode( GL_LIGHTING, 0 );
    ss->setMode( GL_BLEND, 1 );

    // force it to render after the terrain.
    ss->setRenderBinDetails(1, "RenderBin");

    // install the range callback for clip plane activation
    this->addCullCallback( new RangeUniformCullCallback() );
}

void
MGRSGraticule::setMapNode( MapNode* mapNode )
{
    _mapNode = mapNode;
    rebuild();
}

void
MGRSGraticule::rebuild()
{
    applyDefaultStyles();
    
    // clear everything out
    this->removeChildren( 0, this->getNumChildren() );

    // requires a map node
    if ( !getMapNode() )
    {
        return;
    }

    // requires a geocentric map
    if ( !getMapNode()->isGeocentric() )
    {
        OE_WARN << LC << "Projected map mode is not yet supported" << std::endl;
        return;
    }

    const Profile* mapProfile = getMapNode()->getMap()->getProfile();

    _profile = Profile::create(
        mapProfile->getSRS(),
        mapProfile->getExtent().xMin(),
        mapProfile->getExtent().yMin(),
        mapProfile->getExtent().xMax(),
        mapProfile->getExtent().yMax(),
        8, 4 );

    _featureProfile = new FeatureProfile(_profile->getSRS());


    // rebuild the graph:

    // Horizon clipping plane.
    osg::ClipPlane* cp = _clipPlane.get();
    if ( cp )
    {
        _root = this;
    }
    else
    {
        osg::ClipNode* clipNode = new osg::ClipNode();
        osgEarth::Registry::shaderGenerator().run( clipNode );
        cp = new osg::ClipPlane( 0 );
        clipNode->addClipPlane( cp );
        _root = clipNode;
    }
    _root->addCullCallback( new ClipToGeocentricHorizon(_profile->getSRS(), cp) );

    this->addChild( _root );

    // intialize the UTM sector tables for this profile.
    _utmData.rebuild(_profile.get());

    // now build the lateral tiles for the GZD level.
    for( UTMData::SectorTable::iterator i = _utmData.sectorTable().begin(); i != _utmData.sectorTable().end(); ++i )
    {
        osg::Group* group = _utmData.buildGZDTile(i->first, i->second, gzdStyle().get(), _featureProfile.get(), getMapNode()->getMap());
        if ( group )
        { 
            group = buildGZDChildren(group, i->first);
            if (group)
            {
                _root->addChild(group);
            }
        }
    }
}

void
MGRSGraticule::applyDefaultStyles()
{
    if (!gzdStyle().isSet())
    {
        LineSymbol* line = gzdStyle()->getOrCreate<LineSymbol>();
        line->stroke()->color() = Color::Gray;
        line->stroke()->width() = 1.0;
        line->tessellation() = 20;

        TextSymbol* text = gzdStyle()->getOrCreate<TextSymbol>();
        text->fill()->color() = Color(Color::White, 0.3f);
        text->halo()->color() = Color(Color::Black, 0.2f);
        text->alignment() = TextSymbol::ALIGN_CENTER_CENTER;
    }

    if (!sqidStyle().isSet())
    {
        LineSymbol* line = sqidStyle()->getOrCreate<LineSymbol>();
        line->stroke()->color() = Color(Color::White, 0.5f);
        line->stroke()->stipplePattern() = 0x1111;

        TextSymbol* text = sqidStyle()->getOrCreate<TextSymbol>();
        text->fill()->color() = Color(Color::White, 0.3f);
        text->halo()->color() = Color(Color::Black, 0.1f);
        text->alignment() = TextSymbol::ALIGN_CENTER_CENTER;
    }
}

osg::Group*
MGRSGraticule::buildGZDChildren( osg::Group* parent, const std::string& gzd )
{
    osg::BoundingSphere bs = parent->getBound();

    std::string uri = Stringify() << gzd << "." MGRS_GRATICULE_EXTENSION;

    osg::PagedLOD* plod = new osg::PagedLOD();
    plod->setCenter( bs.center() );
    plod->addChild( parent, 0.0, FLT_MAX );
    plod->setFileName( 1, uri );
    plod->setRange( 1, 0, bs.radius() * 10.0 );
    
    // pass a reference to this object through the loader.
    osgDB::Options* readOptions = new osgDB::Options();
    OptionsData<MGRSGraticule>::set(readOptions, "osgEarth.MGRSGraticule", this);
    plod->setDatabaseOptions(readOptions);

    return plod;
}

osg::Node*
MGRSGraticule::buildSQIDTiles( const std::string& gzd )
{
    const GeoExtent& extent = _utmData.sectorTable()[gzd];

    // parse the GZD into its components:
    unsigned zone;
    char letter;
    sscanf( gzd.c_str(), "%u%c", &zone, &letter );
    
    const TextSymbol* textSymFromOptions = sqidStyle()->get<TextSymbol>();
    if ( !textSymFromOptions )
        textSymFromOptions = sqidStyle()->get<TextSymbol>();

    // copy it since we intend to alter it
    osg::ref_ptr<TextSymbol> textSym = 
        textSymFromOptions ? new TextSymbol(*textSymFromOptions) :
        new TextSymbol();

    double h = 0.0;

    TextSymbolizer ts( textSym );
    MGRSFormatter mgrs(MGRSFormatter::PRECISION_100000M);
    osg::Geode* textGeode = new osg::Geode();

    const SpatialReference* ecefSRS = extent.getSRS()->getECEF();
    osg::Vec3d centerMap, centerECEF;
    extent.getCentroid(centerMap.x(), centerMap.y());
    extent.getSRS()->transform(centerMap, ecefSRS, centerECEF);

    osg::Matrix local2world;
    ecefSRS->createLocalToWorld( centerECEF, local2world );
    osg::Matrix world2local;
    world2local.invert(local2world);

    FeatureList features;

    std::vector<GeoExtent> sqidExtents;

    // UTM:
    if ( letter > 'B' && letter < 'Y' )
    {
        // grab the SRS for the current UTM zone:
        // TODO: AL/AA designation??
        const SpatialReference* utm = SpatialReference::create(
            Stringify() << "+proj=utm +zone=" << zone << " +north +units=m" );

        // transform the four corners of the tile to UTM.
        osg::Vec3d gzdUtmSW, gzdUtmSE, gzdUtmNW, gzdUtmNE;
        extent.getSRS()->transform( osg::Vec3d(extent.xMin(),extent.yMin(),h), utm, gzdUtmSW );
        extent.getSRS()->transform( osg::Vec3d(extent.xMin(),extent.yMax(),h), utm, gzdUtmNW );
        extent.getSRS()->transform( osg::Vec3d(extent.xMax(),extent.yMin(),h), utm, gzdUtmSE );
        extent.getSRS()->transform( osg::Vec3d(extent.xMax(),extent.yMax(),h), utm, gzdUtmNE );

        // find the southern boundary of the first full SQID tile in the GZD tile.
        double southSQIDBoundary = gzdUtmSW.y(); //extentUTM.yMin();
        double remainder = fmod( southSQIDBoundary, 100000.0 );
        if ( remainder > 0.0 )
            southSQIDBoundary += (100000.0 - remainder);

        // find the min/max X for this cell in UTM:
        double xmin = extent.yMin() >= 0.0 ? gzdUtmSW.x() : gzdUtmNW.x();
        double xmax = extent.yMin() >= 0.0 ? gzdUtmSE.x() : gzdUtmNE.x();

        // Record the UTM extent of each SQID cell in this tile.
        // Go from the south boundary northwards:
        for( double y = southSQIDBoundary; y < gzdUtmNW.y(); y += 100000.0 )
        {
            // start at the central meridian (500K) and go west:
            for( double x = 500000.0; x > xmin; x -= 100000.0 )
            {
                sqidExtents.push_back( GeoExtent(utm, x-100000.0, y, x, y+100000.0) );
            }

            // then start at the central meridian and go east:
            for( double x = 500000.0; x < xmax; x += 100000.0 )
            {
                sqidExtents.push_back( GeoExtent(utm, x, y, x+100000.0, y+100000.0) );
            }
        }

        for( std::vector<GeoExtent>::iterator i = sqidExtents.begin(); i != sqidExtents.end(); ++i )
        {
            GeoExtent utmEx = *i;

            // now, clamp each of the points in the UTM SQID extent to the map-space
            // boundaries of the GZD tile. (We only need to clamp in the X dimension,
            // Y geometry is allowed to overflow.) Also, skip NE, we don't need it.
            double r, xlimit;

            osg::Vec3d sw(utmEx.xMin(), utmEx.yMin(), 0);
            r = (sw.y()-gzdUtmSW.y())/(gzdUtmNW.y()-gzdUtmSW.y());
            xlimit = gzdUtmSW.x() + r * (gzdUtmNW.x() - gzdUtmSW.x());
            if ( sw.x() < xlimit ) sw.x() = xlimit;

            osg::Vec3d nw(utmEx.xMin(), utmEx.yMax(), 0);
            r = (nw.y()-gzdUtmSW.y())/(gzdUtmNW.y()-gzdUtmSW.y());
            xlimit = gzdUtmSW.x() + r * (gzdUtmNW.x() - gzdUtmSW.x());
            if ( nw.x() < xlimit ) nw.x() = xlimit;
            
            osg::Vec3d se(utmEx.xMax(), utmEx.yMin(), 0);
            r = (se.y()-gzdUtmSE.y())/(gzdUtmNE.y()-gzdUtmSE.y());
            xlimit = gzdUtmSE.x() + r * (gzdUtmNE.x() - gzdUtmSE.x());
            if ( se.x() > xlimit ) se.x() = xlimit;

            // at the northernmost GZD (lateral band X), clamp the northernmost SQIDs to the upper latitude.
            if ( letter == 'X' && nw.y() > gzdUtmNW.y() ) 
                nw.y() = gzdUtmNW.y();

            // need this in order to calculate the font size:
            double utmWidth = se.x() - sw.x();

            // now transform the corner points back into the map SRS:
            utm->transform( sw, extent.getSRS(), sw );
            utm->transform( nw, extent.getSRS(), nw );
            utm->transform( se, extent.getSRS(), se );

            // and draw valid sqid geometry.
            if ( sw.x() < se.x() )
            {
                Feature* lat = new Feature(new LineString(2), extent.getSRS());
                lat->geoInterp() = GEOINTERP_RHUMB_LINE;
                lat->getGeometry()->push_back( sw );
                lat->getGeometry()->push_back( se );
                features.push_back(lat);

                Feature* lon = new Feature(new LineString(2), extent.getSRS());
                lon->geoInterp() = GEOINTERP_GREAT_CIRCLE;
                lon->getGeometry()->push_back( sw );
                lon->getGeometry()->push_back( nw );
                features.push_back(lon);

                // and the text label:
                osg::Vec3d sqidTextMap = (nw + se) * 0.5;
                sqidTextMap.z() += 1000.0;
                osg::Vec3d sqidTextECEF;
                extent.getSRS()->transform(sqidTextMap, ecefSRS, sqidTextECEF);
                osg::Vec3d sqidLocal;
                sqidLocal = sqidTextECEF * world2local;

                MGRSCoord mgrsCoord;
                if ( mgrs.transform( GeoPoint(extent.getSRS(),sqidTextMap,ALTMODE_ABSOLUTE), mgrsCoord) )
                {
                    textSym->size() = utmWidth/3.0;        
                    osgText::Text* d = ts.create( mgrsCoord.sqid );

                    osg::Matrixd textLocal2World;
                    ecefSRS->createLocalToWorld( sqidTextECEF, textLocal2World );

                    d->setPosition( sqidLocal );
                    textGeode->addDrawable( d );
                }
            }
        }
    }

    else if ( letter == 'A' || letter == 'B' )
    {
        // SRS for south polar region UPS projection. This projection has (0,0) at the
        // south pole, with +X extending towards 90 degrees E longitude and +Y towards
        // 0 degrees longitude.
        const SpatialReference* ups = SpatialReference::create(
            "+proj=stere +lat_ts=-90 +lat_0=-90 +lon_0=0 +k_0=1 +x_0=0 +y_0=0");

        osg::Vec3d gtemp;
        double r = GeoMath::distance(-osg::PI_2, 0.0, -1.3962634, 0.0); // -90 => -80 latitude
        double r2 = r*r;

        if ( letter == 'A' )
        {
            for( double x = 0.0; x < 1200000.0; x += 100000.0 )
            {
                double yminmax = sqrt( r2 - x*x );
                Feature* f = new Feature( new LineString(2), extent.getSRS() );
                f->geoInterp() = GEOINTERP_GREAT_CIRCLE;
                osg::Vec3d p0, p1;
                ups->transform( osg::Vec3d(-x, -yminmax, 0), extent.getSRS(), p0 );
                ups->transform( osg::Vec3d(-x,  yminmax, 0), extent.getSRS(), p1 );
                f->getGeometry()->push_back( p0 );
                f->getGeometry()->push_back( p1 );
                features.push_back( f );
            }

            for( double y = -1100000.0; y < 1200000.0; y += 100000.0 )
            {
                double xmax = sqrt( r2 - y*y );
                Feature* f = new Feature( new LineString(2), extent.getSRS() );
                f->geoInterp() = GEOINTERP_GREAT_CIRCLE;
                osg::Vec3d p0, p1;
                ups->transform( osg::Vec3d(-xmax, y, 0), extent.getSRS(), p0 );
                ups->transform( osg::Vec3d(    0, y, 0), extent.getSRS(), p1 );
                f->getGeometry()->push_back( p0 );
                f->getGeometry()->push_back( p1 );
                features.push_back( f );
            }

            for( double x = -1200000.0; x < 0.0; x += 100000.0 )
            {
                for( double y = -1200000.0; y < 1200000.0; y += 100000.0 )
                {
                    osg::Vec3d sqidTextMap;
                    ups->transform( osg::Vec3d(x+50000.0, y+50000.0, 0), extent.getSRS(), sqidTextMap);
                    if ( sqidTextMap.y() < -80.0 )
                    {
                        sqidTextMap.z() += 1000.0;
                        osg::Vec3d sqidTextECEF;
                        extent.getSRS()->transform(sqidTextMap, ecefSRS, sqidTextECEF);
                        osg::Vec3d sqidLocal = sqidTextECEF * world2local;

                        MGRSCoord mgrsCoord;
                        if ( mgrs.transform( GeoPoint(extent.getSRS(),sqidTextMap,ALTMODE_ABSOLUTE), mgrsCoord) )
                        {
                            textSym->size() = 33000.0;
                            osgText::Text* d = ts.create( mgrsCoord.sqid );
                            osg::Matrixd textLocal2World;
                            ecefSRS->createLocalToWorld( sqidTextECEF, textLocal2World );
                            d->setPosition( sqidLocal );
                            textGeode->addDrawable( d );
                        }
                    }
                }
            }
        }

        else if ( letter == 'B' )
        {
            for( double x = 100000.0; x < 1200000.0; x += 100000.0 )
            {
                double yminmax = sqrt( r2 - x*x );
                Feature* f = new Feature( new LineString(2), extent.getSRS() );
                f->geoInterp() = GEOINTERP_GREAT_CIRCLE;
                osg::Vec3d p0, p1;
                ups->transform( osg::Vec3d(x, -yminmax, 0), extent.getSRS(), p0 );
                ups->transform( osg::Vec3d(x,  yminmax, 0), extent.getSRS(), p1 );
                f->getGeometry()->push_back( p0 );
                f->getGeometry()->push_back( p1 );
                features.push_back( f );
            }

            for( double y = -1100000.0; y < 1200000.0; y += 100000.0 )
            {
                double xmax = sqrt( r2 - y*y );
                Feature* f = new Feature( new LineString(2), extent.getSRS() );
                f->geoInterp() = GEOINTERP_GREAT_CIRCLE;
                osg::Vec3d p0, p1;
                ups->transform( osg::Vec3d(    0, y, 0), extent.getSRS(), p0 );
                ups->transform( osg::Vec3d( xmax, y, 0), extent.getSRS(), p1 );
                f->getGeometry()->push_back( p0 );
                f->getGeometry()->push_back( p1 );
                features.push_back( f );
            }

            for( double x = 0.0; x < 1200000.0; x += 100000.0 )
            {
                for( double y = -1200000.0; y < 1200000.0; y += 100000.0 )
                {
                    osg::Vec3d sqidTextMap;
                    ups->transform( osg::Vec3d(x+50000.0, y+50000.0, 0), extent.getSRS(), sqidTextMap);
                    if ( sqidTextMap.y() < -80.0 )
                    {
                        sqidTextMap.z() += 1000.0;
                        osg::Vec3d sqidTextECEF;
                        extent.getSRS()->transform(sqidTextMap, ecefSRS, sqidTextECEF);
                        //extent.getSRS()->transformToECEF(sqidTextMap, sqidTextECEF);
                        osg::Vec3d sqidLocal = sqidTextECEF * world2local;

                        MGRSCoord mgrsCoord;
                        if ( mgrs.transform( GeoPoint(extent.getSRS(),sqidTextMap,ALTMODE_ABSOLUTE), mgrsCoord) )
                        {
                            textSym->size() = 33000.0;
                            osgText::Text* d = ts.create( mgrsCoord.sqid );
                            osg::Matrixd textLocal2World;
                            ecefSRS->createLocalToWorld( sqidTextECEF, textLocal2World );
                            d->setPosition( sqidLocal );
                            textGeode->addDrawable( d );
                        }
                    }
                }
            }
        }
    }

    else if ( letter == 'Y' || letter == 'Z' )
    {
        // SRS for north polar region UPS projection. This projection has (0,0) at the
        // south pole, with +X extending towards 90 degrees E longitude and +Y towards
        // 180 degrees longitude.
        const SpatialReference* ups = SpatialReference::create(
            "+proj=stere +lat_ts=90 +lat_0=90 +lon_0=0 +k_0=1 +x_0=0 +y_0=0");

        osg::Vec3d gtemp;
        double r = GeoMath::distance(osg::PI_2, 0.0, 1.46607657, 0.0); // 90 -> 84 latitude
        double r2 = r*r;

        if ( letter == 'Y' )
        {
            for( double x = 0.0; x < 700000.0; x += 100000.0 )
            {
                double yminmax = sqrt( r2 - x*x );
                Feature* f = new Feature( new LineString(2), extent.getSRS() );
                f->geoInterp() = GEOINTERP_GREAT_CIRCLE;
                osg::Vec3d p0, p1;
                ups->transform( osg::Vec3d(-x, -yminmax, 0), extent.getSRS(), p0 );
                ups->transform( osg::Vec3d(-x,  yminmax, 0), extent.getSRS(), p1 );
                f->getGeometry()->push_back( p0 );
                f->getGeometry()->push_back( p1 );
                features.push_back( f );
            }

            for( double y = -600000.0; y < 700000.0; y += 100000.0 )
            {
                double xmax = sqrt( r2 - y*y );
                Feature* f = new Feature( new LineString(2), extent.getSRS() );
                f->geoInterp() = GEOINTERP_GREAT_CIRCLE;
                osg::Vec3d p0, p1;
                ups->transform( osg::Vec3d(-xmax, y, 0), extent.getSRS(), p0 );
                ups->transform( osg::Vec3d(    0, y, 0), extent.getSRS(), p1 );
                f->getGeometry()->push_back( p0 );
                f->getGeometry()->push_back( p1 );
                features.push_back( f );
            }

            for( double x = -700000.0; x < 0.0; x += 100000.0 )
            {
                for( double y = -700000.0; y < 700000.0; y += 100000.0 )
                {
                    osg::Vec3d sqidTextMap;
                    ups->transform( osg::Vec3d(x+50000.0, y+50000.0, 0), extent.getSRS(), sqidTextMap);
                    if ( sqidTextMap.y() > 84.0 )
                    {
                        sqidTextMap.z() += 1000.0;
                        osg::Vec3d sqidTextECEF;
                        extent.getSRS()->transform(sqidTextMap, ecefSRS, sqidTextECEF);
                        //extent.getSRS()->transformToECEF(sqidTextMap, sqidTextECEF);
                        osg::Vec3d sqidLocal = sqidTextECEF * world2local;

                        MGRSCoord mgrsCoord;
                        if ( mgrs.transform( GeoPoint(extent.getSRS(),sqidTextMap,ALTMODE_ABSOLUTE), mgrsCoord) )
                        {
                            textSym->size() = 33000.0;
                            osgText::Text* d = ts.create( mgrsCoord.sqid );
                            osg::Matrixd textLocal2World;
                            ecefSRS->createLocalToWorld( sqidTextECEF, textLocal2World );
                            d->setPosition( sqidLocal );
                            textGeode->addDrawable( d );
                        }
                    }
                }
            }
        }

        else if ( letter == 'Z' )
        {
            for( double x = 100000.0; x < 700000.0; x += 100000.0 )
            {
                double yminmax = sqrt( r2 - x*x );
                Feature* f = new Feature( new LineString(2), extent.getSRS() );
                f->geoInterp() = GEOINTERP_GREAT_CIRCLE;
                osg::Vec3d p0, p1;
                ups->transform( osg::Vec3d(x, -yminmax, 0), extent.getSRS(), p0 );
                ups->transform( osg::Vec3d(x,  yminmax, 0), extent.getSRS(), p1 );
                f->getGeometry()->push_back( p0 );
                f->getGeometry()->push_back( p1 );
                features.push_back( f );
            }

            for( double y = -600000.0; y < 700000.0; y += 100000.0 )
            {
                double xmax = sqrt( r2 - y*y );
                Feature* f = new Feature( new LineString(2), extent.getSRS() );
                f->geoInterp() = GEOINTERP_GREAT_CIRCLE;
                osg::Vec3d p0, p1;
                ups->transform( osg::Vec3d(    0, y, 0), extent.getSRS(), p0 );
                ups->transform( osg::Vec3d( xmax, y, 0), extent.getSRS(), p1 );
                f->getGeometry()->push_back( p0 );
                f->getGeometry()->push_back( p1 );
                features.push_back( f );
            }

            for( double x = 0.0; x < 700000.0; x += 100000.0 )
            {
                for( double y = -700000.0; y < 700000.0; y += 100000.0 )
                {
                    osg::Vec3d sqidTextMap;
                    ups->transform( osg::Vec3d(x+50000.0, y+50000.0, 0), extent.getSRS(), sqidTextMap);
                    if ( sqidTextMap.y() > 84.0 )
                    {
                        sqidTextMap.z() += 1000.0;
                        osg::Vec3d sqidTextECEF;
                        extent.getSRS()->transform(sqidTextMap, ecefSRS, sqidTextECEF);
                        //extent.getSRS()->transformToECEF(sqidTextMap, sqidTextECEF);
                        osg::Vec3d sqidLocal = sqidTextECEF * world2local;

                        MGRSCoord mgrsCoord;
                        if ( mgrs.transform( GeoPoint(extent.getSRS(),sqidTextMap,ALTMODE_ABSOLUTE), mgrsCoord) )
                        {
                            textSym->size() = 33000.0;
                            osgText::Text* d = ts.create( mgrsCoord.sqid );
                            osg::Matrixd textLocal2World;
                            ecefSRS->createLocalToWorld( sqidTextECEF, textLocal2World );
                            d->setPosition( sqidLocal );
                            textGeode->addDrawable( d );
                        }
                    }
                }
            }
        }
    }

    osg::Group* group = new osg::Group();

    Style lineStyle;
    lineStyle.add( sqidStyle()->get<LineSymbol>() );

    GeometryCompiler compiler;
    osg::ref_ptr<Session> session = new Session( getMapNode()->getMap() );
    FilterContext context( session.get(), _featureProfile.get(), extent );

    // make sure we get sufficient tessellation:
    compiler.options().maxGranularity() = 0.25;

    osg::Node* geomNode = compiler.compile(features, lineStyle, context);
    if ( geomNode ) 
        group->addChild( geomNode );

    osg::MatrixTransform* mt = new osg::MatrixTransform(local2world);
    mt->addChild(textGeode);
    group->addChild( mt );

    Registry::shaderGenerator().run(textGeode, Registry::stateSetCache());

    return group;
}

//---------------------------------------------------------------------------

namespace osgEarth { namespace Util
{
    // OSG Plugin for loading subsequent graticule levels
    class MGRSGraticuleFactory : public osgDB::ReaderWriter
    {
    public:
        MGRSGraticuleFactory()
        {
            supportsExtension( MGRS_GRATICULE_EXTENSION, "osgEarth MGRS graticule" );
        }

        const char* className() const
        {
            return "osgEarth MGRS graticule LOD loader";
        }

        bool acceptsExtension(const std::string& extension) const
        {
            return osgDB::equalCaseInsensitive(extension, MGRS_GRATICULE_EXTENSION);
        }

        ReadResult readNode(const std::string& uri, const Options* options) const
        {        
            std::string ext = osgDB::getFileExtension( uri );
            if ( !acceptsExtension( ext ) )
                return ReadResult::FILE_NOT_HANDLED;

            if ( !options )
            {
                OE_WARN << LC << "INTERNAL ERROR: MGRSGraticule object not present in Options (1)\n";
                return ReadResult::ERROR_IN_READING_FILE;
            }

            osg::ref_ptr<MGRSGraticule> graticule;
            if (!OptionsData<MGRSGraticule>::lock(options, "osgEarth.MGRSGraticule", graticule))
            {
                OE_WARN << LC << "INTERNAL ERROR: MGRSGraticule object not present in Options (2)\n";
                return ReadResult::ERROR_IN_READING_FILE;
            }

            //MGRSGraticule* graticule = const_cast<MGRSGraticule*>(dynamic_cast<const MGRSGraticule*>(udc->getUserObject(MGRS_GRATICULE_OBJECT_NAME)));
            //if (!graticule)
            //{
            //    OE_WARN << LC << "INTERNAL ERROR: MGRSGraticule object not present in Options (3)\n";
            //    return ReadResult::ERROR_IN_READING_FILE;

            //}
            std::string def = osgDB::getNameLessExtension(uri);
            std::string gzd = osgDB::getNameLessExtension(def);
            
            osg::Node* result = graticule->buildSQIDTiles( gzd );

            return result ? ReadResult(result) : ReadResult::ERROR_IN_READING_FILE;
        }
    };
    REGISTER_OSGPLUGIN(MGRS_GRATICULE_EXTENSION, MGRSGraticuleFactory);


    REGISTER_OSGEARTH_EXTENSION(osgearth_mgrs_graticule, MGRSGraticuleExtension);


} } // namespace osgEarth::Util


