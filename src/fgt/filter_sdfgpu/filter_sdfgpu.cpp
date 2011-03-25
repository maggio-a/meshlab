#include "filter_sdfgpu.h"
#include <vcg/complex/trimesh/base.h>
#include <vcg/complex/trimesh/update/topology.h>
#include <vcg/complex/trimesh/update/edges.h>
#include <vcg/complex/trimesh/update/bounding.h>
#include <vcg/complex/trimesh/update/quality.h>
#include <vcg/complex/trimesh/update/color.h>
#include <vcg/complex/trimesh/update/flag.h>
#include <vcg/complex/trimesh/clean.h>
#include <vcg/complex/intersection.h>
#include <vcg/space/index/grid_static_ptr.h>
#include <vcg/space/index/spatial_hashing.h>
#include <vcg/math/matrix33.h>
#include <wrap/qt/to_string.h>
#include <vcg/math/gen_normal.h>
#include <wrap/qt/checkGLError.h>
#include <stdio.h>
#include <assert.h>
using namespace std;
using namespace vcg;


#define SDF_MAX_TEXTURE_SIZE 1024

SdfGpuPlugin::SdfGpuPlugin()
: SingleMeshFilterInterface("Compute SDF GPU"),
  mPeelingTextureSize(256)
{

}

void SdfGpuPlugin::initParameterSet(MeshDocument&, RichParameterSet& par)
{
  qDebug() << "called here!";
  QStringList onPrimitive; onPrimitive.push_back("On vertices"); onPrimitive.push_back("On Faces");
  par.addParam( new RichEnum("onPrimitive", 0, onPrimitive, "Metric:",
                             "Choose whether to trace rays from faces or from vertices. "
                             "Recall that tracing from vertices will use vertex normal "
                             "estimation."));

  par.addParam(  new RichInt("numberRays", 128, "Number of rays: ",
                             "The standard deviation of the rays that will be casted around "
                             "the anti-normals. Remember that most sampled directions are "
                             "expected to fall within 3x this value."));
 /* par.addParam(new RichFloat("lowQuantile", .1, "Bottom quantile",
                             "We will throw away the set of ray distances for each cone which distance "
                             "value falls under this quantile. Value in between [0,1]. 0 Implies all "
                             "values are kept"));
  par.addParam(new RichFloat("hiQuantile", .9, "Top quantile",
                             "We will throw away the set of ray distances for each cone which distance "
                             "value falls under this quantile. Value in between [0,1]. 1 Implies all "
                             "values are kept"));*/

  par.addParam(new RichInt("DepthTextureSize", 512, "Depth texture size",
                             "Size of the depth texture for depth peeling"));

  par.addParam(new RichInt("peelingIteration", 2, "Peeling Iteration",
                             "Number of depth peeling iteration"));

  par.addParam(new RichFloat("peelingTolerance", 0.00005f, "Peeling Tolerance",
                             "We will throw away the set of ray distances for each cone which distance " ));

  par.addParam(new RichFloat("depthTolerance", 0.0001f, "Depth tolerance",
                             "A small delta that is the minimal distance in depth between two vertex" ));

  par.addParam(new RichFloat("minCos", 0.7f, "Min cosine",
                             "Min accepteded cosine of the angle between rays and vertex normals" ));

  par.addParam(new RichFloat("maxCos", 1.0f, "Max cosine",
                             "Max accepteded cosine of the angle between rays and vertex normals" ));
}


bool SdfGpuPlugin::applyFilter(MeshDocument& md, RichParameterSet& pars, vcg::CallBackPos* cb)
{
  MeshModel* mm = md.mm();
  CMeshO&    cm = mm->cm;

  //******* RETRIEVE PARAMETERS **********/
  ONPRIMITIVE  onPrimitive  = (ONPRIMITIVE) pars.getInt("onPrimitive");
  unsigned int numViews     = pars.getInt("numberRays");
  int          peel         = pars.getInt("peelingIteration");
  mTolerance                = pars.getFloat("peelingTolerance");
  mPeelingTextureSize       = pars.getInt("DepthTextureSize");
  mDepthTolerance           = pars.getFloat("depthTolerance");
  mMinCos                   = pars.getFloat("minCos");
  mMaxCos                   = pars.getFloat("maxCos");
  assert( onPrimitive==ON_VERTICES && "Face mode not supported yet" );

   //******* GL & MESH INIT **********/
   setupMesh( md, onPrimitive );
   initGL(cm.vn);
   vertexDataToTexture(*mm);

  //******** CALCULATE SDF *************/
  std::vector<Point3f> unifDirVec;
  GenNormal<float>::Uniform(numViews,unifDirVec);

  for(vector<vcg::Point3f>::iterator vi = unifDirVec.begin(); vi != unifDirVec.end(); vi++)
  {
        (*vi).Normalize();
        TraceRays(peel, mTolerance, *vi, md.mm());
  }

  applySdfHW(*mm,unifDirVec.size());

  //******* CLEAN AND EXIT *************/
  releaseGL();

  return true;
}

bool SdfGpuPlugin::initGL(unsigned int numVertices)
{
    this->glContext->makeCurrent();
    //******* SET DEFAULT OPENGL STUFF **********/
    glEnable( GL_DEPTH_TEST );
    glEnable( GL_TEXTURE_2D );
    glDisable(GL_BLEND);
    glDisable(GL_LIGHTING);
    glEnable(GL_NORMALIZE);
    glDisable(GL_COLOR_MATERIAL);
    glClearColor(0,0,1,0);
    glClearDepth(1.0);

    GLenum err = glewInit();

    if (GLEW_OK != err)
    {
            Log(0,(const char*)glewGetErrorString(err));
            return false;
    }
    //**********INIT FBOs for depth peeling***********
    mFboA      = new FramebufferObject();
    mFboB      = new FramebufferObject();
    mFboResult = new FramebufferObject();

    unsigned int maxTexSize;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, reinterpret_cast<GLint*>(&maxTexSize) );
    Log(0, "QUERY HARDWARE FOR: MAX TEX SIZE: %i ", maxTexSize );

    if ((maxTexSize*maxTexSize) < numVertices)
    {
            Log(0, "That's a really huge model, I can't handle it in hardware, sorry..");
            return false;
    }

    for( mResTextureDim = 16; mResTextureDim*mResTextureDim < numVertices; mResTextureDim *= 2 ){}


    mVertexCoordsTexture  = new FloatTexture2D( TextureFormat( GL_TEXTURE_2D, mResTextureDim, mResTextureDim, GL_RGBA32F_ARB, GL_RGBA, GL_FLOAT ), TextureParams( GL_NEAREST, GL_NEAREST ) );
    mVertexNormalsTexture = new FloatTexture2D( TextureFormat( GL_TEXTURE_2D, mResTextureDim, mResTextureDim, GL_RGBA32F_ARB, GL_RGBA, GL_FLOAT ), TextureParams( GL_NEAREST, GL_NEAREST ) );

    mResultTexture = new FloatTexture2D( TextureFormat( GL_TEXTURE_2D, mResTextureDim, mResTextureDim, GL_RGBA32F_ARB, GL_RGBA, GL_FLOAT ), TextureParams( GL_NEAREST, GL_NEAREST ) );
    mFboResult->attachTexture( mResultTexture->format().target(), mResultTexture->id(), GL_COLOR_ATTACHMENT0_EXT );
    //clear first time
    mFboResult->bind();

    glPushAttrib(GL_COLOR_BUFFER_BIT);
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);
    glPopAttrib();

    mFboResult->unbind();

    mColorTextureA = new FloatTexture2D( TextureFormat( GL_TEXTURE_2D, mPeelingTextureSize, mPeelingTextureSize, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE ), TextureParams( GL_NEAREST, GL_NEAREST ) );
    mDepthTextureA = new FloatTexture2D( TextureFormat( GL_TEXTURE_2D, mPeelingTextureSize, mPeelingTextureSize, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT ), TextureParams( GL_NEAREST, GL_NEAREST ) );
    mColorTextureB = new FloatTexture2D( TextureFormat( GL_TEXTURE_2D, mPeelingTextureSize, mPeelingTextureSize, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE ), TextureParams( GL_NEAREST, GL_NEAREST ) );
    mDepthTextureB = new FloatTexture2D( TextureFormat( GL_TEXTURE_2D, mPeelingTextureSize, mPeelingTextureSize, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT ), TextureParams( GL_NEAREST, GL_NEAREST ) );

    mFboA->attachTexture( mColorTextureA->format().target(), mColorTextureA->id(), GL_COLOR_ATTACHMENT0 );
    mFboA->attachTexture( mDepthTextureA->format().target(), mDepthTextureA->id(), GL_DEPTH_ATTACHMENT  );
    mFboB->attachTexture( mColorTextureB->format().target(), mColorTextureB->id(), GL_COLOR_ATTACHMENT0 );
    mFboB->attachTexture( mDepthTextureB->format().target(), mDepthTextureB->id(), GL_DEPTH_ATTACHMENT  );

    mDeepthPeelingProgram = new GPUProgram("",":/SdfGpu/shaders/shaderDepthPeeling.fs","");
    mDeepthPeelingProgram->enable();
    mDeepthPeelingProgram->addUniform("textureLastDepth");
    mDeepthPeelingProgram->addUniform("tolerance");
    mDeepthPeelingProgram->addUniform("oneOverBufSize");
    mDeepthPeelingProgram->disable();

    mSDFProgram = new GPUProgram("",":/SdfGpu/shaders/calculateSdf.frag","");
    mSDFProgram->enable();
    mSDFProgram->addUniform("vTexture");
    mSDFProgram->addUniform("nTexture");
    mSDFProgram->addUniform("depthTextureFront");
    mSDFProgram->addUniform("depthTextureBack");
    mSDFProgram->addUniform("viewDirection");
    mSDFProgram->addUniform("mvprMatrix");
    mSDFProgram->addUniform("viewpSize");
    mSDFProgram->addUniform("texSize");
    mSDFProgram->disable();

    assert(mFboResult->isValid());
    assert(mFboA->isValid());
    assert(mFboB->isValid());

    checkGLError::qDebug("GL Init failed");
}

void SdfGpuPlugin::vertexDataToTexture(MeshModel &m)
{
        unsigned int texSize = mResTextureDim*mResTextureDim*4;

        GLfloat *vertexPosition= new GLfloat[texSize];
        GLfloat *vertexNormals = new GLfloat[texSize];
        vcg::Point3<CMeshO::ScalarType> vn;

        //Copies each vertex's position and normal in new vectors
        for (int i=0; i < m.cm.vn; ++i)
        {
                //Vertex position
                vertexPosition[i*4+0] = m.cm.vert[i].P().X();
                vertexPosition[i*4+1] = m.cm.vert[i].P().Y();
                vertexPosition[i*4+2] = m.cm.vert[i].P().Z();
                vertexPosition[i*4+3] = 1.0;

                //Normal vector for each vertex
                vn = m.cm.vert[i].N();
                vertexNormals[i*4+0] = vn.X();
                vertexNormals[i*4+1] = vn.Y();
                vertexNormals[i*4+2] = vn.Z();
                vertexNormals[i*4+3] = 1.0;
        }

         //Write vertex coordinates
        mVertexCoordsTexture->bind();
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, mResTextureDim, mResTextureDim, 0, GL_RGBA, GL_FLOAT, vertexPosition);


        //Write normal directions
        mVertexNormalsTexture->bind();
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, mResTextureDim, mResTextureDim, 0, GL_RGBA, GL_FLOAT, vertexNormals);

        delete [] vertexNormals;
        delete [] vertexPosition;
}

void SdfGpuPlugin::releaseGL()
{
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0 );

    delete mDeepthPeelingProgram;
    delete mSDFProgram;
    delete mFboResult;
    delete mFboA;
    delete mFboB;
    delete mResultTexture;
    delete mVertexCoordsTexture;
    delete mVertexNormalsTexture;
    delete mColorTextureA;
    delete mDepthTextureA;
    delete mColorTextureB;
    delete mDepthTextureB;

    checkGLError::qDebug("GL release failed");

    this->glContext->doneCurrent();
}

void SdfGpuPlugin::fillFrameBuffer(bool front,  MeshModel* mm)
{
   (front) ? glClearColor(0,1,0,1) : glClearColor(1,0,0,1);
   glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
   glEnable(GL_CULL_FACE);
   glCullFace((front)?GL_BACK:GL_FRONT);

   mm->glw.DrawFill<GLW::NMPerVert, GLW::CMPerVert, GLW::TMNone>();

   glDisable(GL_CULL_FACE);
}

void SdfGpuPlugin::setupMesh(MeshDocument& md, ONPRIMITIVE onPrimitive )
{
    MeshModel* mm = md.mm();
    CMeshO& m     = mm->cm;

    //--- If on vertices, do some cleaning first
    if( onPrimitive == ON_VERTICES )
    {
      int dup = tri::Clean<CMeshO>::RemoveDuplicateVertex(m);
      int unref =  tri::Clean<CMeshO>::RemoveUnreferencedVertex(m);
      if (dup > 0 || unref > 0) Log("Removed %i duplicate and %i unreferenced vertices\n",dup,unref);
    }

    //--- Updating mesh metadata
    tri::UpdateBounding<CMeshO>::Box(m);
    tri::UpdateNormals<CMeshO>::PerFaceNormalized(m);
    tri::UpdateNormals<CMeshO>::PerVertexAngleWeighted(m);
    tri::UpdateNormals<CMeshO>::NormalizeVertex(m);
    tri::UpdateFlags<CMeshO>::FaceProjection(m);

    //--- Enable & Reset the necessary attributes
    switch(onPrimitive)
    {
      case ON_VERTICES:
        mm->updateDataMask(MeshModel::MM_VERTQUALITY);
        tri::UpdateQuality<CMeshO>::VertexConstant(m,0);
        break;
      case ON_FACES:
        mm->updateDataMask(MeshModel::MM_FACEQUALITY);
        tri::UpdateQuality<CMeshO>::FaceConstant(m,0);
        break;
    }

    //--- Use VBO
  //  mm->glw.SetHint(vcg::GLW::HNUseVBO);
    mm->glw.Update();
}

void SdfGpuPlugin::setCamera(Point3f camDir, Box3f &meshBBox)
{
    GLfloat d = (meshBBox.Diag()/2.0) * 1.1,
            k = 0.1f;
    Point3f eye = meshBBox.Center() + camDir * (d+k);

    glViewport(0.0, 0.0, mPeelingTextureSize, mPeelingTextureSize);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-d, d, -d, d, k, k+(2.0*d) );

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(eye.X(), eye.Y(), eye.Z(),
                      meshBBox.Center().X(), meshBBox.Center().Y(), meshBBox.Center().Z(),
                      0.0, 1.0, 0.0);
}


void SdfGpuPlugin::useDepthPeelingShader(FramebufferObject* fbo)
{
   glUseProgram(mDeepthPeelingProgram->id());
   mDeepthPeelingProgram->setUniform1f("tolerance", mTolerance);
   mDeepthPeelingProgram->setUniform2f("oneOverBufSize", 1.0f/mPeelingTextureSize, 1.0f/mPeelingTextureSize);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, fbo->getAttachedId(GL_DEPTH_ATTACHMENT));
   mDeepthPeelingProgram->setUniform1i("textureLastDepth",0);
}

void SdfGpuPlugin::calculateSdfHW(FramebufferObject& fboFront, FramebufferObject& fboBack, const Point3f& cameraDir)
{
    mFboResult->bind();
    glViewport(0, 0, mResTextureDim, mResTextureDim);
    GLfloat mv_pr_Matrix_f[16];  // modelview-projection matrix

    glGetFloatv(GL_MODELVIEW_MATRIX, mv_pr_Matrix_f);
    glMatrixMode(GL_PROJECTION);
    glMultMatrixf(mv_pr_Matrix_f);
    glGetFloatv(GL_PROJECTION_MATRIX, mv_pr_Matrix_f);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Need to clear the depthBuffer if we don't
    // want a mesh-shaped hole in the middle of the S.A.Q. :)
   // glClear(GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);

    glEnable (GL_BLEND);
    glBlendFunc (GL_ONE, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);

    glUseProgram(mSDFProgram->id());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fboFront.getAttachedId(GL_DEPTH_ATTACHMENT));
    mSDFProgram->setUniform1i("depthTextureFront",0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fboBack.getAttachedId(GL_DEPTH_ATTACHMENT));
    mSDFProgram->setUniform1i("depthTextureBack",1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, mVertexCoordsTexture->id());
    mSDFProgram->setUniform1i("vTexture",2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, mVertexNormalsTexture->id());
    mSDFProgram->setUniform1i("nTexture",3);

    // Set view direction
    mSDFProgram->setUniform3f("viewDirection", cameraDir.X(), cameraDir.Y(), cameraDir.Z());

    // Set ModelView-Projection Matrix
    mSDFProgram->setUniformMatrix4fv( "mvprMatrix", mv_pr_Matrix_f, 1, GL_FALSE );

    // Set texture Size
    mSDFProgram->setUniform1f("texSize", mPeelingTextureSize);

    // Set viewport Size
    mSDFProgram->setUniform1f("viewpSize", mResTextureDim );

    mSDFProgram->setUniform1f("depthTolerance", mDepthTolerance);

//    mSDFProgram->setUniform1f("minCos", 0.7);

//    mSDFProgram->setUniform1f("maxCos", 1.0);


    // Screen-aligned Quad
    glBegin(GL_QUADS);
            glVertex3f(-1.0f, -1.0f, 0.0f); //L-L
            glVertex3f( 1.0f, -1.0f, 0.0f); //L-R
            glVertex3f( 1.0f,  1.0f, 0.0f); //U-R
            glVertex3f(-1.0f,  1.0f, 0.0f); //U-L
    glEnd();

    mFboResult->unbind();
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

}

void SdfGpuPlugin::applySdfHW(MeshModel &m, float numberOfRays)
{
    const unsigned int texelNum = mResTextureDim*mResTextureDim;

    GLfloat *result = new GLfloat[texelNum*4];

    mFboResult->bind();

    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glReadPixels(0, 0, mResTextureDim, mResTextureDim, GL_RGBA, GL_FLOAT, result);

    for (int i=0; i < m.cm.vn; ++i)
    {
        m.cm.vert[i].Q() = (result[i*4+1]>0.0) ? (result[i*4] / result[i*4+1]) : 0.0;
    }

    mFboResult->unbind();

    delete [] result;
}


void SdfGpuPlugin::TraceRays(int peelingIteration, float tolerance, const Point3f& dir, MeshModel* mm )
{
    for( int i = 0;  i < peelingIteration; i++ )
    {
        if( i == 0 )
              glUseProgram(0);
        else
              useDepthPeelingShader(mFboB);


        mFboA->bind();
        setCamera(dir, mm->cm.bbox);

       /* glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(10.0,2.5);*/
        fillFrameBuffer(i%2==0, mm);
        mFboA->unbind();
        //glDisable(GL_POLYGON_OFFSET_FILL);

        if(i%2)
            calculateSdfHW(*mFboB,*mFboA,dir);



        std::swap<FramebufferObject*>(mFboA,mFboB);
   }

    checkGLError::qDebug("Error during depth peeling");
}

Q_EXPORT_PLUGIN(SdfGpuPlugin)