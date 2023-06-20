#include "interactor_impl.h"

#include "animationManager.h"
#include "loader_impl.h"
#include "log.h"
#include "window_impl.h"

#include "vtkF3DConfigure.h"
#include "vtkF3DInteractorEventRecorder.h"
#include "vtkF3DInteractorStyle.h"
#include "vtkF3DRendererWithColoring.h"

#include <vtkCallbackCommand.h>
#include <vtkCellPicker.h>
#include <vtkMath.h>
#include <vtkNew.h>
#include <vtkPointPicker.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRendererCollection.h>
#include <vtkStringArray.h>
#include <vtkVersion.h>
#include <vtksys/SystemTools.hxx>
#include <vtkPicker.h>
#include <vtkTransform.h>

#include <chrono>
#include <cmath>
#include <map>

#include "camera.h"

namespace f3d::detail
{
  //----------------------------------------------------------------------------
  // Method defined to normalize the Z axis so all models are treated temporarily
  // as Z-up axis models. 
  vtkNew<vtkTransform> zUpTransform(vtkRenderer* renderer)
  {
    const double* up = renderer->GetEnvironmentUp();
    const double* right = renderer->GetEnvironmentRight();
    double fwd[3];
    vtkMath::Cross(right, up, fwd);

    const double m[16] = {
      right[0], right[1], right[2], 0, //
      fwd[0], fwd[1], fwd[2], 0,       //
      up[0], up[1], up[2], 0,          //
      0, 0, 0, 1,                      //
    };
    vtkNew<vtkTransform> toZup;
    toZup->SetMatrix(m);
    return toZup;
  }
  vtkNew<vtkTransform> zUpTransform(vtkRenderWindowInteractor* interactor)
  {
    return zUpTransform(interactor->GetRenderWindow()->GetRenderers()->GetFirstRenderer());
  }

  class interactor_impl::internals
  {
  public:
  internals(options& options, window_impl& window, loader_impl& loader)
    : Options(options)
    , Window(window)
    , Loader(loader)
  {
    this->VTKInteractor->SetRenderWindow(this->Window.GetRenderWindow());
    this->VTKInteractor->SetInteractorStyle(this->Style);
    this->VTKInteractor->Initialize();

    // Disable standard interactor behavior with timer event
    // in order to be able to interact while animating
    this->VTKInteractor->RemoveObservers(vtkCommand::TimerEvent);

    vtkNew<vtkCallbackCommand> keyPressCallback;
    keyPressCallback->SetClientData(this);
    keyPressCallback->SetCallback(OnKeyPress);
    this->Style->AddObserver(vtkF3DInteractorStyle::KeyPressEvent, keyPressCallback);

    vtkNew<vtkCallbackCommand> dropFilesCallback;
    dropFilesCallback->SetClientData(this);
    dropFilesCallback->SetCallback(OnDropFiles);
    this->Style->AddObserver(vtkF3DInteractorStyle::DropFilesEvent, dropFilesCallback);

    vtkNew<vtkCallbackCommand> middleButtonPressCallback;
    middleButtonPressCallback->SetClientData(this);
    middleButtonPressCallback->SetCallback(OnMiddleButtonPress);
    this->Style->AddObserver(vtkCommand::MiddleButtonPressEvent, middleButtonPressCallback);

    vtkNew<vtkCallbackCommand> middleButtonReleaseCallback;
    middleButtonReleaseCallback->SetClientData(this);
    middleButtonReleaseCallback->SetCallback(OnMiddleButtonRelease);
    this->Style->AddObserver(vtkCommand::MiddleButtonReleaseEvent, middleButtonReleaseCallback);

// Clear needs https://gitlab.kitware.com/vtk/vtk/-/merge_requests/9229
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 1, 20220601)
    this->Recorder = vtkSmartPointer<vtkF3DInteractorEventRecorder>::New();
    this->Recorder->SetInteractor(this->VTKInteractor);
#endif
  }

  static void OnKeyPress(vtkObject*, unsigned long, void* clientData, void*)
  {
    internals* self = static_cast<internals*>(clientData);
    vtkRenderWindowInteractor* rwi = self->Style->GetInteractor();
    int keyCode = std::toupper(rwi->GetKeyCode());
    std::string keySym = rwi->GetKeySym();
    if (keySym.length() > 0)
    {
      // Make sure key symbols starts with an upper char (e.g. "space")
      keySym[0] = std::toupper(keySym[0]);
    }

    if (self->KeyPressUserCallBack(keyCode, keySym))
    {
      return;
    }

    // No user defined behavior, use standard behavior
    vtkRenderWindow* renWin = self->Window.GetRenderWindow();
    vtkF3DRenderer* ren = vtkF3DRenderer::SafeDownCast(renWin->GetRenderers()->GetFirstRenderer());
    vtkF3DRendererWithColoring* renWithColor = vtkF3DRendererWithColoring::SafeDownCast(ren);
    bool checkColoring = false;
    bool render = false;

    // Declaring variables for camera shortcuts
    const auto toZup = zUpTransform(self->VTKInteractor);
    const auto fromZup = toZup->GetInverse();
    camera& cam = self->Window.getCamera();
    vector3_t up = {0, 0, 1};
    point3_t pos = cam.getPosition();
    point3_t foc = cam.getFocalPoint();
    point3_t A;
    point3_t newPos;
    double dx, dy, dz, radius;

    switch (keyCode)
    {
      case 'C':
        if (renWithColor)
        {
          renWithColor->CycleScalars(vtkF3DRendererWithColoring::CycleType::FIELD);
          self->Window.PrintColoringDescription(log::VerboseLevel::DEBUG);
          checkColoring = true;
          render = true;
        }
        break;
      case 'S':
        if (renWithColor)
        {
          renWithColor->CycleScalars(vtkF3DRendererWithColoring::CycleType::ARRAY_INDEX);
          self->Window.PrintColoringDescription(log::VerboseLevel::DEBUG);
          checkColoring = true;
          render = true;
        }
        break;
      case 'Y':
        if (renWithColor)
        {
          renWithColor->CycleScalars(vtkF3DRendererWithColoring::CycleType::COMPONENT);
          self->Window.PrintColoringDescription(log::VerboseLevel::DEBUG);
          checkColoring = true;
          render = true;
        }
        break;
      case 'B':
        self->Options.toggle("ui.bar");
        render = true;
        break;
      case 'p':
      case 'P':
        self->Options.toggle("render.effect.translucency-support");
        render = true;
        break;
      case 'Q':
        self->Options.toggle("render.effect.ambient-occlusion");
        render = true;
        break;
      case 'A':
        self->Options.toggle("render.effect.anti-aliasing");
        render = true;
        break;
      case 'T':
        self->Options.toggle("render.effect.tone-mapping");
        render = true;
        break;
      case 'E':
        self->Options.toggle("render.show-edges");
        render = true;
        break;
      case 'X':
        self->Options.toggle("interactor.axis");
        render = true;
        break;
      case 'G':
        self->Options.toggle("render.grid.enable");
        render = true;
        break;
      case 'N':
        self->Options.toggle("ui.filename");
        render = true;
        break;
      case 'M':
        self->Options.toggle("ui.metadata");
        render = true;
        break;
      case 'Z':
        self->Options.toggle("ui.fps");
        self->Window.render();
        self->Window.render();
        // XXX: Double render is needed here
        break;
      case 'R':
        self->Options.toggle("render.raytracing.enable");
        render = true;
        break;
      case 'D':
        self->Options.toggle("render.raytracing.denoise");
        render = true;
        break;
      case 'V':
        self->Options.toggle("model.volume.enable");
        render = true;
        break;
      case 'I':
        self->Options.toggle("model.volume.inverse");
        render = true;
        break;
      case 'O':
        self->Options.toggle("model.point-sprites.enable");
        render = true;
        break;
      case 'U':
        self->Options.toggle("render.background.blur");
        render = true;
        break;
      case 'K':
        self->Options.toggle("interactor.trackball");
        render = true;
        break;
      case 'L':
      {
        const double intensity = self->Options.getAsDouble("render.light.intensity");
        const bool down = rwi->GetShiftKey();

        /* `ref < x` is equivalent to:
         * - `intensity <= x` when going down
         * - `intensity < x` when going up */
        const double ref = down ? intensity - 1e-6 : intensity;
        // clang-format off
        /* offset in percentage points */
        const int offsetPp = ref < .5 ?  1
                           : ref <  1 ?  2
                           : ref <  5 ?  5
                           : ref < 10 ? 10
                           :            25;
        // clang-format on
        /* new intensity in percents */
        const int newIntensityPct = std::lround(intensity * 100) + (down ? -offsetPp : +offsetPp);

        self->Options.set("render.light.intensity", std::max(newIntensityPct, 0) / 100.0);
        render = true;
        break;
      }
      case 'H':
        self->Options.toggle("ui.cheatsheet");
        render = true;
        break;
      case '?':
        self->Window.PrintColoringDescription(log::VerboseLevel::INFO);
        self->Window.PrintSceneDescription(log::VerboseLevel::INFO);
        break;
      case '1':
        log::info("--------------------------");
        log::info("Debug: Front View");
        log::info("cam pos = ", pos[0], " ", pos[1], " ", pos[2]);
        log::info("cam foc = ", foc[0], " ", foc[1], " ", foc[2]);

        self->setViewOrbit("Front");
        render = true;

        log::info("cam radius = ", radius);
        log::info("cam A = ", A[0], " ", A[1], " ", A[2]);
        log::info("cam newPos = ", newPos[0], " ", newPos[1], " ", newPos[2]);
        log::info("--------------------------");
        break;
      case '3':
        log::info("--------------------------");
        log::info("Debug: Right View");
        log::info("cam pos = ", pos[0], " ", pos[1], " ", pos[2]);
        log::info("cam foc = ", foc[0], " ", foc[1], " ", foc[2]);

        dx = foc[0] - pos[0]; 
        dy = foc[1] - pos[1]; 
        dz = foc[2] - pos[2]; 
        radius = sqrt(dx*dx + dy*dy + dz*dz);
        A = {1, 0, 0};
        newPos[0] = foc[0] + radius * A[0];
        newPos[1] = foc[1] + radius * A[1];
        newPos[2] = foc[2] + radius * A[2];
        cam.setPosition(newPos);
        cam.setViewUp({0, 1, 0 });
        cam.resetToBounds();
        render = true;

        log::info("cam radius = ", radius);
        log::info("cam A = ", A[0], " ", A[1], " ", A[2]);
        log::info("cam newPos = ", newPos[0], " ", newPos[1], " ", newPos[2]);
        log::info("--------------------------");

        break;
      case '7':
        log::info("--------------------------");
        log::info("Debug: Right View");
        log::info("cam pos = ", pos[0], " ", pos[1], " ", pos[2]);
        log::info("cam foc = ", foc[0], " ", foc[1], " ", foc[2]);

        dx = foc[0] - pos[0]; 
        dy = foc[1] - pos[1]; 
        dz = foc[2] - pos[2]; 
        radius = sqrt(dx*dx + dy*dy + dz*dz);
        A = {0, 1, 0};
        newPos[0] = foc[0] + radius * A[0];
        newPos[1] = foc[1] + radius * A[1];
        newPos[2] = foc[2] + radius * A[2];
        cam.setPosition(newPos);
        cam.setViewUp({0, 0, -1});
        cam.resetToBounds();
        render = true;

        log::info("cam radius = ", radius);
        log::info("cam A = ", A[0], " ", A[1], " ", A[2]);
        log::info("cam newPos = ", newPos[0], " ", newPos[1], " ", newPos[2]);
        log::info("--------------------------");
        break;
      default:
        if (keySym == F3D_EXIT_HOTKEY_SYM)
        {
          self->StopInteractor();
        }
        else if (keySym == "Return")
        {
          self->Window.getCamera().resetToDefault();
          render = true;
        }
        else if (keySym == "Space")
        {
          assert(self->AnimationManager);
          self->AnimationManager->ToggleAnimation();
        }
        break;
    }

    if (checkColoring)
    {
      // Resynchronise renderer coloring status with options
      self->Options.set("model.scivis.cells", renWithColor->GetColoringUseCell());
      self->Options.set("model.scivis.array-name", renWithColor->GetColoringArrayName());
      self->Options.set("model.scivis.component", renWithColor->GetColoringComponent());
    }
    if (render)
    {
      self->Window.render();
    }
  }

  static void OnDropFiles(vtkObject*, unsigned long, void* clientData, void* callData)
  {
    internals* self = static_cast<internals*>(clientData);
    vtkStringArray* filesArr = static_cast<vtkStringArray*>(callData);
    std::vector<std::string> filesVec;
    filesVec.resize(filesArr->GetNumberOfTuples());
    for (int i = 0; i < filesArr->GetNumberOfTuples(); i++)
    {
      filesVec[i] = filesArr->GetValue(i);
    }

    if (self->DropFilesUserCallBack(filesVec))
    {
      return;
    }

    // No user defined behavior, load the first file
    if (!filesVec.empty())
    {
      assert(self->AnimationManager);
      self->AnimationManager->StopAnimation();
      if (self->Loader.hasSceneReader(filesVec[0]))
      {
        self->Loader.loadScene(filesVec[0]);
      }
      else if (self->Loader.hasGeometryReader(filesVec[0]))
      {
        self->Loader.loadGeometry(filesVec[0], true);
      }
      self->Window.render();
    }
  }

  static void OnMiddleButtonPress(vtkObject*, unsigned long, void* clientData, void*)
  {
    internals* self = static_cast<internals*>(clientData);

    self->VTKInteractor->GetEventPosition(self->MiddleButtonDownPosition);

    self->Style->OnMiddleButtonDown();
  }

  static void OnMiddleButtonRelease(vtkObject*, unsigned long, void* clientData, void*)
  {
    internals* self = static_cast<internals*>(clientData);

    const int* middleButtonUpPosition = self->VTKInteractor->GetEventPosition();

    const int xDelta = middleButtonUpPosition[0] - self->MiddleButtonDownPosition[0];
    const int yDelta = middleButtonUpPosition[1] - self->MiddleButtonDownPosition[1];
    const int sqPosDelta = xDelta * xDelta + yDelta * yDelta;
    if (sqPosDelta < self->DragDistanceTol * self->DragDistanceTol)
    {
      const int x = self->MiddleButtonDownPosition[0];
      const int y = self->MiddleButtonDownPosition[1];
      vtkRenderer* renderer =
        self->VTKInteractor->GetRenderWindow()->GetRenderers()->GetFirstRenderer();

      bool pickSuccessful = false;
      double picked[3];
      self->CellPicker->Pick(x, y, 0, renderer);
      if (self->CellPicker->GetActors()->GetNumberOfItems() > 0)
      {
        self->CellPicker->GetPickPosition(picked);
        pickSuccessful = true;
      }
      else
      {
        self->PointPicker->Pick(x, y, 0, renderer);
        if (self->PointPicker->GetActors()->GetNumberOfItems() > 0)
        {
          self->PointPicker->GetPickPosition(picked);
          pickSuccessful = true;
        }
      }

      if (pickSuccessful)
      {
        /*     pos.--------------------.foc
         *       /|                   /
         *      / |                  /
         *     .--.-----------------.picked
         * pos1    pos2
         */
        camera& cam = self->Window.getCamera();
        const point3_t pos = cam.getPosition();
        const point3_t foc = cam.getFocalPoint();

        double focV[3];
        vtkMath::Subtract(picked, foc.data(), focV); /* foc -> picked */

        double posV[3];
        vtkMath::Subtract(picked, foc.data(), posV); /* pos -> pos1, parallel to focV */
        if (!self->Style->GetInteractor()->GetShiftKey())
        {
          double v[3];
          vtkMath::Subtract(foc.data(), pos.data(), v); /* pos -> foc */
          vtkMath::ProjectVector(focV, v, v);           /* pos2 -> pos1 */
          vtkMath::Subtract(posV, v, posV);             /* pos -> pos2, keeps on camera plane */
        }

        if (self->TransitionDuration > 0)
        {
          const auto start = std::chrono::high_resolution_clock::now();
          const auto end = start + std::chrono::milliseconds(self->TransitionDuration);
          auto now = start;
          while (now < end)
          {
            const double t =
              std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() /
              static_cast<double>(self->TransitionDuration);
            const double u = (1 - std::cos(vtkMath::Pi() * t)) / 2;

            cam.setFocalPoint({ foc[0] + focV[0] * u, foc[1] + focV[1] * u, foc[2] + focV[2] * u });
            cam.setPosition({ pos[0] + posV[0] * u, pos[1] + posV[1] * u, pos[2] + posV[2] * u });
            self->Window.render();

            now = std::chrono::high_resolution_clock::now();
          }
        }

        cam.setFocalPoint({ picked[0], picked[1], picked[2] });
        cam.setPosition({ pos[0] + posV[0], pos[1] + posV[1], pos[2] + posV[2] });
        self->Window.render();
      }
    }

    self->Style->OnMiddleButtonUp();
  }

  std::function<bool(int, const std::string&)> KeyPressUserCallBack = [](int, const std::string&)
  { return false; };
  std::function<bool(const std::vector<std::string>&)> DropFilesUserCallBack =
    [](const std::vector<std::string>&) { return false; };

  void StartInteractor()
  {
    this->VTKInteractor->Start();
  }

  void StopInteractor()
  {
    this->VTKInteractor->RemoveObservers(vtkCommand::TimerEvent);
    this->VTKInteractor->ExitCallback();
  }

  options& Options;
  window_impl& Window;
  loader_impl& Loader;
  animationManager* AnimationManager;

  vtkNew<vtkRenderWindowInteractor> VTKInteractor;
  vtkNew<vtkF3DInteractorStyle> Style;
  vtkSmartPointer<vtkF3DInteractorEventRecorder> Recorder;
  int WindowSize[2] = { -1, -1 };
  int WindowPos[2] = { 0, 0 };
  std::map<unsigned long, std::pair<int, std::function<void()> > > TimerCallBacks;

  vtkNew<vtkCellPicker> CellPicker;
  vtkNew<vtkPointPicker> PointPicker;

  int MiddleButtonDownPosition[2] = { 0, 0 };

  int DragDistanceTol = 3;      /* px */
  int TransitionDuration = 100; /* ms */
};

//----------------------------------------------------------------------------
interactor_impl::interactor_impl(options& options, window_impl& window, loader_impl& loader)
  : Internals(std::make_unique<interactor_impl::internals>(options, window, loader))
{
  // Loader need the interactor, loader will set the AnimationManager on the interactor
  this->Internals->Loader.SetInteractor(this);
}

//----------------------------------------------------------------------------
interactor_impl::~interactor_impl() = default;

//----------------------------------------------------------------------------
interactor& interactor_impl::setKeyPressCallBack(std::function<bool(int, std::string)> callBack)
{
  this->Internals->KeyPressUserCallBack = callBack;
  return *this;
}

//----------------------------------------------------------------------------
interactor& interactor_impl::setDropFilesCallBack(
  std::function<bool(std::vector<std::string>)> callBack)
{
  this->Internals->DropFilesUserCallBack = callBack;
  return *this;
}

//----------------------------------------------------------------------------
void interactor_impl::removeTimerCallBack(unsigned long id)
{
  this->Internals->VTKInteractor->RemoveObserver(id);
  this->Internals->VTKInteractor->DestroyTimer(this->Internals->TimerCallBacks[id].first);
}

//----------------------------------------------------------------------------
unsigned long interactor_impl::createTimerCallBack(double time, std::function<void()> callBack)
{
  // Create the timer
  int timerId = this->Internals->VTKInteractor->CreateRepeatingTimer(time);

  // Create the callback and get the observer id
  vtkNew<vtkCallbackCommand> timerCallBack;
  timerCallBack->SetCallback(
    [](vtkObject*, unsigned long, void* clientData, void*)
    {
      std::function<void()>* callBackPtr = static_cast<std::function<void()>*>(clientData);
      (*callBackPtr)();
    });
  unsigned long id =
    this->Internals->VTKInteractor->AddObserver(vtkCommand::TimerEvent, timerCallBack);

  // Store the user callback and set it as client data
  this->Internals->TimerCallBacks[id] = std::make_pair(timerId, callBack);
  timerCallBack->SetClientData(&this->Internals->TimerCallBacks[id].second);
  return id;
}

//----------------------------------------------------------------------------
void interactor_impl::toggleAnimation()
{
  assert(this->Internals->AnimationManager);
  this->Internals->AnimationManager->ToggleAnimation();
}

//----------------------------------------------------------------------------
void interactor_impl::startAnimation()
{
  assert(this->Internals->AnimationManager);
  this->Internals->AnimationManager->StartAnimation();
}

//----------------------------------------------------------------------------
void interactor_impl::stopAnimation()
{
  assert(this->Internals->AnimationManager);
  this->Internals->AnimationManager->StopAnimation();
}

//----------------------------------------------------------------------------
bool interactor_impl::isPlayingAnimation()
{
  assert(this->Internals->AnimationManager);
  return this->Internals->AnimationManager->IsPlaying();
}

//----------------------------------------------------------------------------
void interactor_impl::enableCameraMovement()
{
  this->Internals->Style->SetCameraMovementDisabled(false);
}

//----------------------------------------------------------------------------
void interactor_impl::disableCameraMovement()
{
  this->Internals->Style->SetCameraMovementDisabled(true);
}

//----------------------------------------------------------------------------
bool interactor_impl::playInteraction(const std::string& file)
{
  if (!vtksys::SystemTools::FileExists(file))
  {
    log::error("Interaction record file to play does not exist ", file);
    return false;
  }
  else
  {
// Clear needs https://gitlab.kitware.com/vtk/vtk/-/merge_requests/9229
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 1, 20220601)
    // Make sure the recorder is off and streams are cleared
    this->Internals->Recorder->Off();
    this->Internals->Recorder->Clear();
#else
    // Create a clean recorder as its internal state matters
    this->Internals->Recorder = vtkSmartPointer<vtkF3DInteractorEventRecorder>::New();
    this->Internals->Recorder->SetInteractor(this->Internals->VTKInteractor);
#endif

    std::string cleanFile = vtksys::SystemTools::CollapseFullPath(file);
    this->Internals->Recorder->SetFileName(cleanFile.c_str());
    this->Internals->Window.UpdateDynamicOptions();
    this->Internals->Recorder->Play();
  }

  // Recorder can stop the interactor, make sure it is still running
  if (this->Internals->VTKInteractor->GetDone())
  {
    log::error("Interactor has been stopped");
    return false;
  }
  return true;
}

//----------------------------------------------------------------------------
bool interactor_impl::recordInteraction(const std::string& file)
{
  if (file.empty())
  {
    // TODO improve VTK to check file opening
    log::error("No interaction record file provided");
    return false;
  }

// Clear needs https://gitlab.kitware.com/vtk/vtk/-/merge_requests/9229
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 1, 20220601)
  // Make sure the recorder is off and streams are cleared
  this->Internals->Recorder->Off();
  this->Internals->Recorder->Clear();
#else
  // Create a clean recorder as its internal state matters
  this->Internals->Recorder = vtkSmartPointer<vtkF3DInteractorEventRecorder>::New();
  this->Internals->Recorder->SetInteractor(this->Internals->VTKInteractor);
#endif

  std::string cleanFile = vtksys::SystemTools::CollapseFullPath(file);
  this->Internals->Recorder->SetFileName(cleanFile.c_str());
  this->Internals->Recorder->On();
  this->Internals->Recorder->Record();

  return true;
}

//----------------------------------------------------------------------------
void interactor_impl::start()
{
  this->Internals->Window.UpdateDynamicOptions();
  this->Internals->StartInteractor();
}

//----------------------------------------------------------------------------
void interactor_impl::stop()
{
  this->Internals->StopInteractor();
}

//----------------------------------------------------------------------------
void interactor_impl::SetAnimationManager(animationManager* manager)
{
  this->Internals->AnimationManager = manager;
}
//----------------------------------------------------------------------------
void interactor_impl::SetInteractorOn(vtkInteractorObserver* observer)
{
  observer->SetInteractor(this->Internals->VTKInteractor);
}

//----------------------------------------------------------------------------
void interactor_impl::UpdateRendererAfterInteraction()
{
  this->Internals->Style->UpdateRendererAfterInteraction();
}

//----------------------------------------------------------------------------
void interactor_impl::setViewOrbit(const std::string& view)
{
  const auto toZup = zUpTransform(self->VTKInteractor);
  const auto fromZup = toZup->GetInverse();
  camera& cam = self->Window.getCamera();
  vector3_t up = {0, 0, 1};
  point3_t pos = cam.getPosition();
  point3_t foc = cam.getFocalPoint();
  point3_t A;

  /* convert coords to +Z up */
  toZup->TransformPoint(pos.data(), pos.data());
  toZup->TransformPoint(foc.data(), foc.data());

  const double dx = foc[0] - pos[0]; 
  const double dy = foc[1] - pos[1]; 
  const double dz = foc[2] - pos[2]; 
  double radius = sqrt(dx*dx + dy*dy + dz*dz);

  switch(view) 
    {
      case 'Front':
        A = {0, 0, 1};
      case 'Right':
        A = {1, 0, 0};
      case 'Top':
        A = {0, 1, 0};
    }
  fromZup->TransformPoint(A.data(), A.data());
  point3_t newPos[0] = foc[0] + radius * A[0];
  point3_t newPos[1] = foc[1] + radius * A[1];
  point3_t newPos[2] = foc[2] + radius * A[2];

  /* convert coords back to whatever up is according to model/options */
  fromZup->TransformPoint(newPos.data(), newPos.data());
  fromZup->TransformPoint(foc.data(), foc.data());
  fromZup->TransformPoint(up.data(), up.data());

  /* set camera coordinates back */
  cam.setPosition(newPos);
  cam.setViewUp(up);
  cam.resetToBounds(0.9);
}
}
