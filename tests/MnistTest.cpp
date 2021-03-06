#define BOOST_TEST_MODULE ML_CONVNET_MNIST
#include <shark/Data/Dataset.h>
#include <shark/Data/Csv.h>
#include <shark/Data/DataView.h>

#include <shark/Rng/Normal.h>
#include <shark/Rng/DiscreteUniform.h>

#include <shark/ObjectiveFunctions/Loss/CrossEntropy.h> // loss during training
#include <shark/ObjectiveFunctions/Loss/ZeroOneLoss.h> //loss for test performance
#include <shark/ObjectiveFunctions/Loss/SquaredLoss.h>
#include <shark/ObjectiveFunctions/ErrorFunction.h> //error function to connect data model and loss
#include <shark/ObjectiveFunctions/NoisyErrorFunction.h> //error function to connect data model and loss
#include <shark/ObjectiveFunctions/Regularizer.h> //L1 and L2 regularisation

#include <shark/Algorithms/GradientDescent/SteepestDescent.h>
#include <shark/Algorithms/GradientDescent/LBFGS.h>
#include <shark/Algorithms/GradientDescent/Rprop.h> //resilient propagation as optimizer
// #include <shark/Algorithms/GradientDescent/NoisyRprop.h> //resilient propagation as optimizer

#include <boost/test/unit_test.hpp>
#include <boost/test/floating_point_comparison.hpp>
#include <boost/timer/timer.hpp>

// #include "shark/Test/Models/derivativeTestHelper.h"

#include "cudnn.h"
#include "cuda_profiler_api.h"

#include "derivativeTestHelper.h"

//#include "test_helpers.h"
#include "../src/layers/AbstractLayer.h"
#include "../src/layers/InputLayer.h"
#include "../src/layers/ConvolutionalLayer.h"
#include "../src/layers/FullyConnectedLayer.h"
#include "../src/layers/PoolingLayer.h"
#include "../src/layers/ActivationLayer.h"
#include "../src/layers/SoftmaxLayer.h"
#include "../src/layers/DistortLayer.h"
#include "../src/ConvNet.h"

#include <time.h>

// uncomment to disable assert()
// #define NDEBUG
#include <cassert>


using namespace shark;

BOOST_AUTO_TEST_SUITE (Models_ConvNet_mnist)

#define EPSILON 1.e-6
#define EST_EPSILON 1.e-7

BOOST_AUTO_TEST_CASE( CONVNET_mnist_shark)
{
	// ################ FILES ######################
	// Filenames
	time_t rawtime;
	struct tm * timeinfo;
	char fname_error_shadow_train [80];  // We write to this file
	char fname_error_train [80];  // and copy to this file, to avoid unlycky read/writes from the outside.
	char fname_error_shadow_test [80];  // We write to this file
	char fname_error_test [80];  // and copy to this file, to avoid unlycky read/writes from the outside.
	char fname_weights [80];

	time (&rawtime);
	timeinfo = localtime (&rawtime);
	
	//                                          %m%d%H%M%S = month-day-hour-minute-second
	strftime (fname_error_shadow_train, 80, "out/shadow_mnist_error_train_%m-%d-%H-%M-%S.csv", timeinfo);
	strftime (fname_error_shadow_test, 80, "out/shadow_mnist_error_test_%m-%d-%H-%M-%S.csv", timeinfo);
	strftime (fname_error_train, 80, "out/mnist_error_train_%m-%d-%H-%M-%S.csv", timeinfo);
	strftime (fname_error_test, 80, "out/mnist_error_test_%m-%d-%H-%M-%S.csv", timeinfo);
	strftime (fname_weights, 80, "out/mnist_weights_%m-%d-%H-%M-%S.csv", timeinfo);
	
	std::cout << "writing to "  << fname_weights << std::endl;
	std::cout << "writing to "  << fname_error_train << std::endl;
	std::cout << "writing to "  << fname_error_test << std::endl;
	
	std::ofstream ofs_shadow_train(fname_error_shadow_train);
	std::ofstream ofs_shadow_test(fname_error_shadow_test);
	// CSV-headers:
	ofs_shadow_train << "Iteration, CrossEntropy Train" << std::endl;
	ofs_shadow_test << "Iteration, 01 Test" << std::endl;
	// #############################################
	
	// #############################################
	// DATA
	// #############################################
	// Load data, use 70% for training and 30% for testing.
	// The path is hard coded; make sure to invoke the executable
	// from a place where the data file can be found. It is located
	// under [shark]/examples/Supervised/data.
	LabeledData<RealVector, unsigned int> traindata;
	LabeledData<RealVector, unsigned int> testdata;
	
	size_t batch_size = 64;
	
	// Testdata
	{  // ~ 90 secs on mnist_train.csv
		// ~ 452.38 secs on mnist_train_aug.csv
		// ~18.s on mnist_train_aug.csv shark-release flag: -O3
		boost::timer::auto_cpu_timer t(2, "%w secs\n");
		importCSV(testdata, "data/mnist/mnist_test_normed.csv", shark::FIRST_COLUMN, ',', '#', 1024);
		// traindata = splitAtElement(testdata, 0.83333 * testdata.numberOfElements());
		std::cout << "Time to load test data from csv: ";
	}
	
	// // Traindata
	{  // ~ 90 secs on mnist_train.csv
		// ~ 452.38 secs on mnist_train_aug.csv
		// ~18.s on mnist_train_aug.csv shark-release flag: -O3
		boost::timer::auto_cpu_timer t(2, "%w secs\n");
		importCSV(traindata, "data/mnist/mnist_train_normed.csv", shark::FIRST_COLUMN, ',', '#', batch_size);
		std::cout << "Time to load train data from csv: ";
	}

	std::cout << "Traindata elements: " << traindata.numberOfElements() << std::endl
						<< "Testdata elements: " << testdata.numberOfElements() << std::endl
						<< "Traindata batch_size: " << batch_size << std::endl;
	// traindata.shuffle();  // No shuffle to recreate Simard et al.
	// #############################################
	
	// #############################################
	// Convnet setup
	// #############################################
	int INPUT_SIZE = 28;  // MNIST is 28x28 pixels and 1 channel
	typedef double PREC;
	std::vector<AbstractLayer<PREC>*> layers;
	AbstractLayer<PREC> *lay = NULL;
	layers.push_back(new InputLayer<PREC>(1, INPUT_SIZE, INPUT_SIZE));
	// lay = new DistortLayer<PREC>(3, 30,  // Max-distort, max-rotate
	//                              3, 9);  // max-elastic distort, gauss kernel-size
	// layers.push_back(lay);
	
	layers.push_back(new ConvolutionalLayer<PREC>(20, 5, 5,  // 5 feat maps, 5x5
																								0, 0,  // pad
																								1, 1));  // stride. res 11 x 11
	layers.push_back(new ActivationLayer<PREC>(CUDNN_ACTIVATION_RELU));
	layers.push_back(new PoolingLayer<PREC>(CUDNN_POOLING_MAX, 2,2, 0,0, 2,2));

	layers.push_back(new ConvolutionalLayer<PREC>(50, 5, 5,  // 50 feat maps, 5x5
																								1, 1,  // pad
																								1, 1));  // stride. res 6x6
	layers.push_back(new ActivationLayer<PREC>(CUDNN_ACTIVATION_RELU));
	layers.push_back(new PoolingLayer<PREC>(CUDNN_POOLING_MAX, 2,2, 0,0, 2,2));

	layers.push_back(new FullyConnectedLayer<PREC>(500));  // 1x100
	layers.push_back(new ActivationLayer<PREC>(CUDNN_ACTIVATION_RELU));
	// layers.push_back(new ActivationLayer<PREC>(CUDNN_ACTIVATION_SIGMOID));

	layers.push_back(new FullyConnectedLayer<PREC>(10));  // 1x10
	layers.push_back(new SoftmaxLayer<PREC>());

	ConvNet<PREC> convnet (0);
	convnet.setStructure(layers, (size_t) std::max(1024, (int) batch_size));
	
	convnet.print_info(std::cout);
	
	
	// // Weight init (xavier, uniform or normal)
	convnet.xavier_init();
	// Rng::globalRng.seed(time(NULL));
	// initRandomUniform(convnet, -0.1, 0.1);
	// initRandomNormal(convnet, 0.005);
	// #############################################
	
	// #############################################
	// Training setup
	// #############################################
	// Loss'es
	ZeroOneLoss<unsigned int, RealVector> loss01;
	CrossEntropy loss; // surrogate loss for training

	//create error function
	NoisyErrorFunction error(traindata, &convnet, &loss, batch_size);
	
	// SGD
	double learnrate = 0.01;
	SteepestDescent optimizer;
	optimizer.setLearningRate(learnrate);
	optimizer.setMomentum(0.9);
	optimizer.init(error, convnet.parameterVector());
	
	// #############################################
	
	// #############################################
	// Actual training
	// #############################################
	int steps = 2000000;
	int validate_interval = 500;
	
	PREC lossval;
	PREC lossval01;
	
	boost::timer::auto_cpu_timer time_overall(2, "%w secs\n");  // Total training time
	boost::timer::auto_cpu_timer time_iter(2, "%w secs\n");
	for (int i = 0; i < steps; i++) {
		if (!(i % validate_interval)) {
			time_iter.stop();
			std::cout << "Iter: " << i
			<< " Optim val: " << optimizer.solution().value
			<< "  Avg. iter-time: "
			<< ((float) time_iter.elapsed().wall / validate_interval) / 10.e9
			<< " secs" << std::endl;

			{  // Eval train-error on entire dataset
				boost::timer::auto_cpu_timer t(2, "%w secs\n");
				// ##########################
				// Train-eval CrossEntropy
				// if (lay)
				// 	lay->m_test_eval = true;
				// lossval = loss(traindata.labels(), convnet(traindata.inputs()));
				// if (lay)
				// 	lay->m_test_eval = false;
				// ##########################
				lossval = optimizer.solution().value;
				std::cout << "  " << loss.name() << ": " << lossval << " time: ";
				ofs_shadow_train << i << ", " << lossval << std::endl;
				// Copy the file
				std::ifstream source_train(fname_error_shadow_train, std::ios::binary);
				std::ofstream dest_train(fname_error_train, std::ios::binary);
				dest_train << source_train.rdbuf();
				source_train.close();
				dest_train.close();
			}
			
			{
				boost::timer::auto_cpu_timer t(2, "%w secs\n");
				// Test-eval 0-1-Loss
				if (lay)
					lay->m_test_eval = true;
				lossval01 = loss01(testdata.labels(), convnet(testdata.inputs()));
				if (lay)
					lay->m_test_eval = false;
				std::cout << "  " << loss01.name() << ": " << lossval01 << " time: ";
				ofs_shadow_test << i << ", " << lossval01 << std::endl;
				// Copy the file
				std::ifstream source_test(fname_error_shadow_test, std::ios::binary);
				std::ofstream dest_test(fname_error_test, std::ios::binary);
				dest_test << source_test.rdbuf();
				source_test.close();
				dest_test.close();
			}
			
			time_iter.start();
		}

		optimizer.step(error);
		
		// if (! (i % 100)) {
		// 	boost::timer::auto_cpu_timer t(2, "%w secs\n");
		// 	// Write the weights to disk
		// 	ofstream ofs_weights(fname_weights);
		// 	boost::archive::text_oarchive oa(ofs_weights);
		// 	FloatVector hejs = optimizer.solution().point;
		// 	oa << hejs;
		// 	// optimizer.init(error);
		// 	traindata.shuffle();
		// 	std::cout << "writing, and reshuffleing time: ";
		// 	// optimizer.setMinDelta(1.e-4);
		// }
		if (!(i % 25000) && i != 0) {
				learnrate *= 0.95;
				optimizer.setLearningRate(learnrate);
		}
	}
	// #############################################
	
	// #############################################
	// Eval
	// #############################################
	{
		boost::timer::auto_cpu_timer t(2, "%w secs\n");
		// Eval 0-1-Loss
		if (lay)
			lay->m_test_eval = true;
		lossval01 = loss01(testdata.labels(), convnet(testdata.inputs()));
		// ofs_shadow_train << ", " << lossval01 << std::endl << std::flush;
		std::cout << " " << lossval01 << std::endl;
		std::cout << "01eval time: ";
	}
	
	ofs_shadow_train.close();
	ofs_shadow_test.close();
	std::cout << std::endl;
	std::cout << "Done. Time: ";
	// #############################################
}

BOOST_AUTO_TEST_SUITE_END()
