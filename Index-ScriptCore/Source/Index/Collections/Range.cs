using System.Numerics;

namespace Index.Collections;

public struct Range<T> where T : struct, INumber<T>
{
    private T min;

    public T Min {
        set
        {
            if (value > Max) throw new ArgumentOutOfRangeException($"Range({value},{Max}) is wrong, min can't be more than max.");
            min = value;
        }
        get
        {
            return min;
        }
    }

    private T max;

    public T Max
    {
        set
        {
            if (value < Min) throw new ArgumentOutOfRangeException($"Range({Min},{value}) is wrong, max can't be less than min.");
            max = value;
        }
        get
        {
            return max;
        }
    }

    public Range(T min, T max)
    {
        if(min > max) throw new ArgumentOutOfRangeException($"Range({min},{max}) is wrong, min can't be more than max.");

        // Assign the backing fields directly: the property setters cross-validate
        // against the *other* bound, which is still default(T) (0) mid-construction,
        // so going through them would reject any range with min > 0 (or max < 0).
        // The min > max guard above already validated the pair.
        this.min = min;
        this.max = max;
    }

    public T Clamp(T value)
    {
        if (value < Min) return Min;
        if (value > Max) return Max;
        return value;
    }

    public bool OutOfBounds(T value)
        => value < Min || value > Max;
    
    public bool OutOfBounds(params T[] values)
    {
       for(int i = 0; i < values.Length; i++)
            if (!OutOfBounds(values[i])) return false;

        return true;
    }

    public override string ToString()
    {
        return $"Range({Min}, {Max})";
    }
}
