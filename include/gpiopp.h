
namespace util {

	enum class Mode {
		INPUT,
		OUTPUT
	};

	class GPIO {
	private:
		int pin;
		Mode mode;

	public:
		GPIO() = default;

		GPIO(int pin, Mode mode);

		GPIO& operator=(bool val);

		GPIO& operator=(const GPIO& in);
		
		operator bool() const;
	};
}
